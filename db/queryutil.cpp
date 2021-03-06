// queryutil.cpp

/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "pch.h"

#include "btree.h"
#include "matcher.h"
#include "pdfile.h"
#include "queryoptimizer.h"
#include "../util/unittest.h"

namespace mongo {
    /** returns a string that when used as a matcher, would match a super set of regex()
        returns "" for complex regular expressions
        used to optimize queries in some simple regex cases that start with '^'

        if purePrefix != NULL, sets it to whether the regex can be converted to a range query
    */
    string simpleRegex(const char* regex, const char* flags, bool* purePrefix){
        string r = "";

        if (purePrefix) *purePrefix = false;

        bool multilineOK;
        if ( regex[0] == '\\' && regex[1] == 'A'){
            multilineOK = true;
            regex += 2;
        } else if (regex[0] == '^') {
            multilineOK = false;
            regex += 1;
        } else {
            return r;
        }

        bool extended = false;
        while (*flags){
            switch (*(flags++)){
                case 'm': // multiline
                    if (multilineOK)
                        continue;
                    else
                        return r;
                case 'x': // extended
                    extended = true;
                    break;
                default:
                    return r; // cant use index
            }
        }

        stringstream ss;

        while(*regex){
            char c = *(regex++);
            if ( c == '*' || c == '?' ){
                // These are the only two symbols that make the last char optional
                r = ss.str();
                r = r.substr( 0 , r.size() - 1 );
                return r; //breaking here fails with /^a?/
            } else if (c == '\\'){
                // slash followed by non-alphanumeric represents the following char
                c = *(regex++);
                if ((c >= 'A' && c <= 'Z') ||
                    (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '0') ||
                    (c == '\0'))
                {
                    r = ss.str();
                    break;
                } else {
                    ss << c;
                }
            } else if (strchr("^$.[|()+{", c)){
                // list of "metacharacters" from man pcrepattern
                r = ss.str();
                break;
            } else if (extended && c == '#'){
                // comment
                r = ss.str();
                break;
            } else if (extended && isspace(c)){
                continue;
            } else {
                // self-matching char
                ss << c;
            }
        }

        if ( r.empty() && *regex == 0 ){
            r = ss.str();
            if (purePrefix) *purePrefix = !r.empty();
        }

        return r;
    }
    inline string simpleRegex(const BSONElement& e){
        switch(e.type()){
            case RegEx:
                return simpleRegex(e.regex(), e.regexFlags());
            case Object:{
                BSONObj o = e.embeddedObject();
                return simpleRegex(o["$regex"].valuestrsafe(), o["$options"].valuestrsafe());
            }
            default: assert(false); return ""; //return squashes compiler warning
        }
    }

    string simpleRegexEnd( string regex ) {
        ++regex[ regex.length() - 1 ];
        return regex;
    }    
    
    
    FieldRange::FieldRange( const BSONElement &e, bool isNot, bool optimize ) {
        // NOTE with $not, we could potentially form a complementary set of intervals.
        if ( !isNot && !e.eoo() && e.type() != RegEx && e.getGtLtOp() == BSONObj::opIN ) {
            set< BSONElement, element_lt > vals;
            vector< FieldRange > regexes;
            uassert( 12580 , "invalid query" , e.isABSONObj() );
            BSONObjIterator i( e.embeddedObject() );
            while( i.more() ) {
                BSONElement ie = i.next();
                if ( ie.type() == RegEx ) {
                    regexes.push_back( FieldRange( ie, false, optimize ) );
                } else {
                    vals.insert( ie );
                }
            }

            for( set< BSONElement, element_lt >::const_iterator i = vals.begin(); i != vals.end(); ++i )
                _intervals.push_back( FieldInterval(*i) );

            for( vector< FieldRange >::const_iterator i = regexes.begin(); i != regexes.end(); ++i )
                *this |= *i;
            
            return;
        }
        
        if ( e.type() == Array && e.getGtLtOp() == BSONObj::Equality ){
            
            _intervals.push_back( FieldInterval(e) );
            
            const BSONElement& temp = e.embeddedObject().firstElement();
            if ( ! temp.eoo() ){
                if ( temp < e )
                    _intervals.insert( _intervals.begin() , temp );
                else
                    _intervals.push_back( FieldInterval(temp) );
            }
            
            return;
        }

        _intervals.push_back( FieldInterval() );
        FieldInterval &initial = _intervals[ 0 ];
        BSONElement &lower = initial._lower._bound;
        bool &lowerInclusive = initial._lower._inclusive;
        BSONElement &upper = initial._upper._bound;
        bool &upperInclusive = initial._upper._inclusive;
        lower = minKey.firstElement();
        lowerInclusive = true;
        upper = maxKey.firstElement();
        upperInclusive = true;

        if ( e.eoo() )
            return;
        if ( e.type() == RegEx
             || (e.type() == Object && !e.embeddedObject()["$regex"].eoo())
           )
        {
            if ( !isNot ) { // no optimization for negated regex - we could consider creating 2 intervals comprising all nonmatching prefixes
                const string r = simpleRegex(e);
                if ( r.size() ) {
                    lower = addObj( BSON( "" << r ) ).firstElement();
                    upper = addObj( BSON( "" << simpleRegexEnd( r ) ) ).firstElement();
                    upperInclusive = false;
                } else {
                    BSONObjBuilder b1(32), b2(32);
                    b1.appendMinForType( "" , String );
                    lower = addObj( b1.obj() ).firstElement();

                    b2.appendMaxForType( "" , String );
                    upper = addObj( b2.obj() ).firstElement();
                    upperInclusive = false; //MaxForType String is an empty Object
                }

                // regex matches self - regex type > string type
                if (e.type() == RegEx){
                    BSONElement re = addObj( BSON( "" << e ) ).firstElement();
                    _intervals.push_back( FieldInterval(re) );
                } else {
                    BSONObj orig = e.embeddedObject();
                    BSONObjBuilder b;
                    b.appendRegex("", orig["$regex"].valuestrsafe(), orig["$options"].valuestrsafe());
                    BSONElement re = addObj( b.obj() ).firstElement();
                    _intervals.push_back( FieldInterval(re) );
                }

            }
            return;
        }
        int op = e.getGtLtOp();
        if ( isNot ) {
            switch( op ) {
                case BSONObj::Equality:
                case BSONObj::opALL:
                case BSONObj::opMOD: // NOTE for mod and type, we could consider having 1-2 intervals comprising the complementary types (multiple intervals already possible with $in)
                case BSONObj::opTYPE:
                    op = BSONObj::NE; // no bound calculation
                    break;
                case BSONObj::NE:
                    op = BSONObj::Equality;
                    break;
                case BSONObj::LT:
                    op = BSONObj::GTE;
                    break;
                case BSONObj::LTE:
                    op = BSONObj::GT;
                    break;
                case BSONObj::GT:
                    op = BSONObj::LTE;
                    break;
                case BSONObj::GTE:
                    op = BSONObj::LT;
                    break;
                default: // otherwise doesn't matter
                    break;
            }
        }
        switch( op ) {
        case BSONObj::Equality:
            lower = upper = e;
            break;
        case BSONObj::LT:
            upperInclusive = false;
        case BSONObj::LTE:
            upper = e;
            break;
        case BSONObj::GT:
            lowerInclusive = false;
        case BSONObj::GTE:
            lower = e;
            break;
        case BSONObj::opALL: {
            massert( 10370 ,  "$all requires array", e.type() == Array );
            BSONObjIterator i( e.embeddedObject() );
            bool bound = false;
            while ( i.more() ){
                BSONElement x = i.next();
                if ( x.type() == Object && x.embeddedObject().firstElement().getGtLtOp() == BSONObj::opELEM_MATCH ){
                    // taken care of elsewhere
                }
                else if ( x.type() != RegEx ) {
                    lower = upper = x;
                    bound = true;
                    break;
                }
            }
            if ( !bound ) { // if no good non regex bound found, try regex bounds
                BSONObjIterator i( e.embeddedObject() );
                while( i.more() ) {
                    BSONElement x = i.next();
                    if ( x.type() != RegEx )
                        continue;
                    string simple = simpleRegex( x.regex(), x.regexFlags() );
                    if ( !simple.empty() ) {
                        lower = addObj( BSON( "" << simple ) ).firstElement();
                        upper = addObj( BSON( "" << simpleRegexEnd( simple ) ) ).firstElement();
                        break;
                    }
                }
            }
            break;
        }
        case BSONObj::opMOD: {
            {
                BSONObjBuilder b;
                b.appendMinForType( "" , NumberDouble );
                lower = addObj( b.obj() ).firstElement();
            }
            {
                BSONObjBuilder b;
                b.appendMaxForType( "" , NumberDouble );
                upper = addObj( b.obj() ).firstElement();
            }            
            break;
        }
        case BSONObj::opTYPE: {
            BSONType t = (BSONType)e.numberInt();
            {
                BSONObjBuilder b;
                b.appendMinForType( "" , t );
                lower = addObj( b.obj() ).firstElement();
            }
            {
                BSONObjBuilder b;
                b.appendMaxForType( "" , t );
                upper = addObj( b.obj() ).firstElement();
            }
            
            break;
        }
        case BSONObj::opREGEX:
        case BSONObj::opOPTIONS:
            // do nothing
            break;
        case BSONObj::opELEM_MATCH: {
            log() << "warning: shouldn't get here?" << endl;
            break;
        }
        case BSONObj::opNEAR:
        case BSONObj::opWITHIN:
            _special = "2d";
            break;
        default:
            break;
        }
        
        if ( optimize ){
            if ( lower.type() != MinKey && upper.type() == MaxKey && lower.isSimpleType() ){ // TODO: get rid of isSimpleType
                BSONObjBuilder b;
                b.appendMaxForType( lower.fieldName() , lower.type() );
                upper = addObj( b.obj() ).firstElement();
            }
            else if ( lower.type() == MinKey && upper.type() != MaxKey && upper.isSimpleType() ){ // TODO: get rid of isSimpleType
                BSONObjBuilder b;
                b.appendMinForType( upper.fieldName() , upper.type() );
                lower = addObj( b.obj() ).firstElement();
            }
        }

    }

    void FieldRange::finishOperation( const vector< FieldInterval > &newIntervals, const FieldRange &other ) {
        _intervals = newIntervals;
        for( vector< BSONObj >::const_iterator i = other._objData.begin(); i != other._objData.end(); ++i )
            _objData.push_back( *i );
        if ( _special.size() == 0 && other._special.size() )
            _special = other._special;
    }
    
    // as called, these functions find the max/min of a bound in the
    // opposite direction, so inclusive bounds are considered less
    // superlative
    FieldBound maxFieldBound( const FieldBound &a, const FieldBound &b ) {
        int cmp = a._bound.woCompare( b._bound, false );
        if ( ( cmp == 0 && !b._inclusive ) || cmp < 0 )
            return b;
        return a;
    }

    FieldBound minFieldBound( const FieldBound &a, const FieldBound &b ) {
        int cmp = a._bound.woCompare( b._bound, false );
        if ( ( cmp == 0 && !b._inclusive ) || cmp > 0 )
            return b;
        return a;
    }

    bool fieldIntervalOverlap( const FieldInterval &one, const FieldInterval &two, FieldInterval &result ) {
        result._lower = maxFieldBound( one._lower, two._lower );
        result._upper = minFieldBound( one._upper, two._upper );
        return result.valid();
    }
    
	// NOTE Not yet tested for complex $or bounds, just for simple bounds generated by $in
    const FieldRange &FieldRange::operator&=( const FieldRange &other ) {
        vector< FieldInterval > newIntervals;
        vector< FieldInterval >::const_iterator i = _intervals.begin();
        vector< FieldInterval >::const_iterator j = other._intervals.begin();
        while( i != _intervals.end() && j != other._intervals.end() ) {
            FieldInterval overlap;
            if ( fieldIntervalOverlap( *i, *j, overlap ) )
                newIntervals.push_back( overlap );
            if ( i->_upper == minFieldBound( i->_upper, j->_upper ) )
                ++i;
            else
                ++j;      
        }
        finishOperation( newIntervals, other );
        return *this;
    }
    
    void handleInterval( const FieldInterval &lower, FieldBound &low, FieldBound &high, vector< FieldInterval > &newIntervals ) {
        if ( low._bound.eoo() ) {
            low = lower._lower; high = lower._upper;
        } else {
            if ( high._bound.woCompare( lower._lower._bound, false ) < 0 ) { // when equal but neither inclusive, just assume they overlap, since current btree scanning code just as efficient either way
                FieldInterval tmp;
                tmp._lower = low;
                tmp._upper = high;
                newIntervals.push_back( tmp );
                low = lower._lower; high = lower._upper;                    
            } else {
                high = lower._upper;
            }
        }        
    }
    
    const FieldRange &FieldRange::operator|=( const FieldRange &other ) {
        vector< FieldInterval > newIntervals;
        FieldBound low;
        FieldBound high;
        vector< FieldInterval >::const_iterator i = _intervals.begin();
        vector< FieldInterval >::const_iterator j = other._intervals.begin();
        while( i != _intervals.end() && j != other._intervals.end() ) {
            int cmp = i->_lower._bound.woCompare( j->_lower._bound, false );
            if ( ( cmp == 0 && i->_lower._inclusive ) || cmp < 0 ) {
                handleInterval( *i, low, high, newIntervals );
                ++i;
            } else {
                handleInterval( *j, low, high, newIntervals );
                ++j;
            } 
        }
        while( i != _intervals.end() ) {
            handleInterval( *i, low, high, newIntervals );
            ++i;            
        }
        while( j != other._intervals.end() ) {
            handleInterval( *j, low, high, newIntervals );
            ++j;            
        }
        FieldInterval tmp;
        tmp._lower = low;
        tmp._upper = high;
        newIntervals.push_back( tmp );        
        finishOperation( newIntervals, other );
        return *this;        
    }
    
    const FieldRange &FieldRange::operator-=( const FieldRange &other ) {
        vector< FieldInterval >::iterator i = _intervals.begin();
        vector< FieldInterval >::const_iterator j = other._intervals.begin();
        while( i != _intervals.end() && j != other._intervals.end() ) {
            int cmp = i->_lower._bound.woCompare( j->_lower._bound, false );
            if ( cmp < 0 ||
                ( cmp == 0 && i->_lower._inclusive && !j->_lower._inclusive ) ) {
                int cmp2 = i->_upper._bound.woCompare( j->_lower._bound, false );
                if ( cmp2 < 0 ) {
                    ++i;
                } else if ( cmp2 == 0 ) {
                    if ( i->_upper._inclusive && j->_lower._inclusive ) {
                        i->_upper._inclusive = false;
                    }
                    ++i;
                } else {
                    int cmp3 = i->_upper._bound.woCompare( j->_upper._bound, false );
                    if ( cmp3 < 0 ||
                        ( cmp3 == 0 && ( !i->_upper._inclusive || j->_upper._inclusive ) ) ) {
                        i->_upper = j->_lower;
                        i->_upper.flipInclusive();
                        ++i;
                    } else {
                        ++j;
                    }
                }
            } else {
                int cmp2 = i->_lower._bound.woCompare( j->_upper._bound, false );
                if ( cmp2 > 0 ||
                    ( cmp2 == 0 && ( !i->_lower._inclusive || !j->_lower._inclusive ) ) ) {
                    ++j;
                } else {
                    int cmp3 = i->_upper._bound.woCompare( j->_upper._bound, false );
                    if ( cmp3 < 0 ||
                        ( cmp3 == 0 && ( !i->_upper._inclusive || j->_upper._inclusive ) ) ) {
                        i = _intervals.erase( i );
                    } else {
                        i->_lower = j->_upper;
                        i->_lower.flipInclusive();                        
                        ++j;
                    }
                }                
            }
        }
        finishOperation( _intervals, other );
        return *this;        
    }
    
    BSONObj FieldRange::addObj( const BSONObj &o ) {
        _objData.push_back( o );
        return o;
    }
    
    BSONObj FieldRange::simplifiedComplex() const {
        BSONObjBuilder bb;
        BSONArrayBuilder a;
        set< string > regexLow;
        set< string > regexHigh;
        for( vector< FieldInterval >::const_iterator i = _intervals.begin(); i != _intervals.end(); ++i ) {
            // this recovers exact $in fields and regexes - should be everything for equality
            if ( i->equality() ) {
                a << i->_upper._bound;
                // right now btree cursor doesn't do exclusive bounds so we need to match end of the regex range
                if ( i->_upper._bound.type() == RegEx ) {
                    string r = simpleRegex( i->_upper._bound );
                    if ( !r.empty() ) {
                        regexLow.insert( r );
                        string re = simpleRegexEnd( r );
                        regexHigh.insert( re );
                        a << re;
                    }
                }
            }
        }
        BSONArray in = a.arr();
        if ( !in.isEmpty() ) {
            bb << "$in" << in;
        }
        BSONObj low;
        BSONObj high;
        // should only be one non regex ineq range
        for( vector< FieldInterval >::const_iterator i = _intervals.begin(); i != _intervals.end(); ++i ) {
            if ( !i->equality() ) {
                if ( !i->_lower._inclusive || i->_lower._bound.type() != String || !regexLow.count( i->_lower._bound.valuestr() ) ) {
                    BSONObjBuilder b;
                    // in btree impl lower bound always inclusive
                    b.appendAs( i->_lower._bound, "$gte" );
                    low = b.obj();
                }
                if ( i->_upper._inclusive || i->_upper._bound.type() != String || !regexHigh.count( i->_upper._bound.valuestr() ) ) {
                    BSONObjBuilder b;
                    // in btree impl upper bound always
                    b.appendAs( i->_upper._bound, "$lte" );
                    high = b.obj();
                }
            }
        }
        if ( !low.isEmpty() ) {
            bb.appendElements( low );
        }
        if ( !high.isEmpty() ) {
            bb.appendElements( high );
        }
        return bb.obj();        
    }
    
    string FieldRangeSet::getSpecial() const {
        string s = "";
        for ( map<string,FieldRange>::iterator i=_ranges.begin(); i!=_ranges.end(); i++ ){
            if ( i->second.getSpecial().size() == 0 )
                continue;
            uassert( 13033 , "can't have 2 special fields" , s.size() == 0 );
            s = i->second.getSpecial();
        }
        return s;
    }

    void FieldRangeSet::processOpElement( const char *fieldName, const BSONElement &f, bool isNot, bool optimize ) {
        BSONElement g = f;
        int op2 = g.getGtLtOp();
        if ( op2 == BSONObj::opALL ) {
            BSONElement h = g;
            massert( 13050 ,  "$all requires array", h.type() == Array );
            BSONObjIterator i( h.embeddedObject() );
            if( i.more() ) {
                BSONElement x = i.next();
                if ( x.type() == Object && x.embeddedObject().firstElement().getGtLtOp() == BSONObj::opELEM_MATCH ) {
                    g = x.embeddedObject().firstElement();
                    op2 = g.getGtLtOp();
                }
            }
        }
        if ( op2 == BSONObj::opELEM_MATCH ) {
            BSONObjIterator k( g.embeddedObjectUserCheck() );
            while ( k.more() ){
                BSONElement h = k.next();
                StringBuilder buf(32);
                buf << fieldName << "." << h.fieldName();
                string fullname = buf.str();
                
                int op3 = getGtLtOp( h );
                if ( op3 == BSONObj::Equality ){
                    _ranges[ fullname ] &= FieldRange( h , isNot , optimize );
                }
                else {
                    BSONObjIterator l( h.embeddedObject() );
                    while ( l.more() ){
                        _ranges[ fullname ] &= FieldRange( l.next() , isNot , optimize );
                    }
                }
            }                        
        } else {
            _ranges[ fieldName ] &= FieldRange( f , isNot , optimize );
        }        
    }
    
    void FieldRangeSet::processQueryField( const BSONElement &e, bool optimize ) {
        bool equality = ( getGtLtOp( e ) == BSONObj::Equality );
        if ( equality && e.type() == Object ) {
            equality = ( strcmp( e.embeddedObject().firstElement().fieldName(), "$not" ) != 0 );
        }
        
        if ( equality || ( e.type() == Object && !e.embeddedObject()[ "$regex" ].eoo() ) ) {
            _ranges[ e.fieldName() ] &= FieldRange( e , false , optimize );
        }
        if ( !equality ) {
            BSONObjIterator j( e.embeddedObject() );
            while( j.more() ) {
                BSONElement f = j.next();
                if ( strcmp( f.fieldName(), "$not" ) == 0 ) {
                    switch( f.type() ) {
                        case Object: {
                            BSONObjIterator k( f.embeddedObject() );
                            while( k.more() ) {
                                BSONElement g = k.next();
                                uassert( 13034, "invalid use of $not", g.getGtLtOp() != BSONObj::Equality );
                                processOpElement( e.fieldName(), g, true, optimize );
                            }
                            break;
                        }
                        case RegEx:
                            processOpElement( e.fieldName(), f, true, optimize );
                            break;
                        default:
                            uassert( 13041, "invalid use of $not", false );
                    }
                } else {
                    processOpElement( e.fieldName(), f, false, optimize );
                }
            }                
        }   
    }
    
    FieldRangeSet::FieldRangeSet( const char *ns, const BSONObj &query , bool optimize )
        : _ns( ns ), _query( query.getOwned() ) {
            BSONObjIterator i( _query );
            
            while( i.more() ) {
                BSONElement e = i.next();
                // e could be x:1 or x:{$gt:1}
                
                if ( strcmp( e.fieldName(), "$where" ) == 0 ) {
                    continue;
                }
                
                if ( strcmp( e.fieldName(), "$or" ) == 0 ) {                                                                                                                                                        
                    continue;
                }
                
                if ( strcmp( e.fieldName(), "$nor" ) == 0 ) {
                    continue;
                }
                
                processQueryField( e, optimize );
            }   
        }

    FieldRangeOrSet::FieldRangeOrSet( const char *ns, const BSONObj &query , bool optimize )
        : _baseSet( ns, query, optimize ), _orFound() {

        BSONObjIterator i( query );
        
        while( i.more() ) {
            BSONElement e = i.next();
            if ( strcmp( e.fieldName(), "$or" ) == 0 ) {                                                                                                                                                        
                massert( 13262, "$or requires nonempty array", e.type() == Array && e.embeddedObject().nFields() > 0 );                                                                                         
                BSONObjIterator j( e.embeddedObject() );                                                                                                                                                        
                while( j.more() ) {                                                                                                                                                                             
                    BSONElement f = j.next();                                                                                                                                                                   
                    massert( 13263, "$or array must contain objects", f.type() == Object );                                                                                                                     
                    _orSets.push_back( FieldRangeSet( ns, f.embeddedObject(), optimize ) );
                    massert( 13291, "$or may not contain 'special' query", _orSets.back().getSpecial().empty() );
                }
                _orFound = true;
                continue;
            }
        }
    }

    FieldRange *FieldRangeSet::trivialRange_ = 0;
    FieldRange &FieldRangeSet::trivialRange() {
        if ( trivialRange_ == 0 )
            trivialRange_ = new FieldRange();
        return *trivialRange_;
    }
    
    BSONObj FieldRangeSet::simplifiedQuery( const BSONObj &_fields, bool expandIn ) const {
        BSONObj fields = _fields;
        if ( fields.isEmpty() ) {
            BSONObjBuilder b;
            for( map< string, FieldRange >::const_iterator i = _ranges.begin(); i != _ranges.end(); ++i ) {
                b.append( i->first.c_str(), 1 );
            }
            fields = b.obj();
        }
        BSONObjBuilder b;
        BSONObjIterator i( fields );
        while( i.more() ) {
            BSONElement e = i.next();
            const char *name = e.fieldName();
            const FieldRange &range = _ranges[ name ];
            assert( !range.empty() );
            if ( range.equality() )
                b.appendAs( range.min(), name );
            else if ( range.nontrivial() ) {
                BSONObj o;
                if ( expandIn ) {
                    o = range.simplifiedComplex();
                } else {
                    BSONObjBuilder c;
                    if ( range.min().type() != MinKey )
                        c.appendAs( range.min(), range.minInclusive() ? "$gte" : "$gt" );
                    if ( range.max().type() != MaxKey )
                        c.appendAs( range.max(), range.maxInclusive() ? "$lte" : "$lt" );
                    o = c.obj();
                }
                b.append( name, o );
            }
        }
        return b.obj();
    }
    
    QueryPattern FieldRangeSet::pattern( const BSONObj &sort ) const {
        QueryPattern qp;
        for( map< string, FieldRange >::const_iterator i = _ranges.begin(); i != _ranges.end(); ++i ) {
            assert( !i->second.empty() );
            if ( i->second.equality() ) {
                qp._fieldTypes[ i->first ] = QueryPattern::Equality;
            } else if ( i->second.nontrivial() ) {
                bool upper = i->second.max().type() != MaxKey;
                bool lower = i->second.min().type() != MinKey;
                if ( upper && lower )
                    qp._fieldTypes[ i->first ] = QueryPattern::UpperAndLowerBound;
                else if ( upper )
                    qp._fieldTypes[ i->first ] = QueryPattern::UpperBound;
                else if ( lower )
                    qp._fieldTypes[ i->first ] = QueryPattern::LowerBound;                    
            }
        }
        qp.setSort( sort );
        return qp;
    }
    
    BoundList FieldRangeSet::indexBounds( const BSONObj &keyPattern, int direction ) const {
        typedef vector< pair< shared_ptr< BSONObjBuilder >, shared_ptr< BSONObjBuilder > > > BoundBuilders;
        BoundBuilders builders;
        builders.push_back( make_pair( shared_ptr< BSONObjBuilder >( new BSONObjBuilder() ), shared_ptr< BSONObjBuilder >( new BSONObjBuilder() ) ) );        
        BSONObjIterator i( keyPattern );
        bool ineq = false; // until ineq is true, we are just dealing with equality and $in bounds
        while( i.more() ) {
            BSONElement e = i.next();
            const FieldRange &fr = range( e.fieldName() );
            int number = (int) e.number(); // returns 0.0 if not numeric
            bool forward = ( ( number >= 0 ? 1 : -1 ) * ( direction >= 0 ? 1 : -1 ) > 0 );
            if ( !ineq ) {
                if ( fr.equality() ) {
                    for( BoundBuilders::const_iterator j = builders.begin(); j != builders.end(); ++j ) {
                        j->first->appendAs( fr.min(), "" );
                        j->second->appendAs( fr.min(), "" );
                    }
                } else {
                    if ( !fr.inQuery() ) {
                        ineq = true;
                    }
                    BoundBuilders newBuilders;
                    const vector< FieldInterval > &intervals = fr.intervals();
                    for( BoundBuilders::const_iterator i = builders.begin(); i != builders.end(); ++i ) {
                        BSONObj first = i->first->obj();
                        BSONObj second = i->second->obj();
                        if ( forward ) {
                            for( vector< FieldInterval >::const_iterator j = intervals.begin(); j != intervals.end(); ++j ) {
                                uassert( 13303, "combinatorial limit of $in partitioning of result set exceeded", newBuilders.size() < 1000000 );
                                newBuilders.push_back( make_pair( shared_ptr< BSONObjBuilder >( new BSONObjBuilder() ), shared_ptr< BSONObjBuilder >( new BSONObjBuilder() ) ) );
                                newBuilders.back().first->appendElements( first );
                                newBuilders.back().second->appendElements( second );
                                newBuilders.back().first->appendAs( j->_lower._bound, "" );
                                newBuilders.back().second->appendAs( j->_upper._bound, "" );
                            }
                        } else {
                            for( vector< FieldInterval >::const_reverse_iterator j = intervals.rbegin(); j != intervals.rend(); ++j ) {
                                uassert( 13304, "combinatorial limit of $in partitioning of result set exceeded", newBuilders.size() < 1000000 );
                                newBuilders.push_back( make_pair( shared_ptr< BSONObjBuilder >( new BSONObjBuilder() ), shared_ptr< BSONObjBuilder >( new BSONObjBuilder() ) ) );
                                newBuilders.back().first->appendElements( first );
                                newBuilders.back().second->appendElements( second );
                                newBuilders.back().first->appendAs( j->_upper._bound, "" );
                                newBuilders.back().second->appendAs( j->_lower._bound, "" );
                            }
                        }
                    }
                    builders = newBuilders;
                }
            } else {
                for( BoundBuilders::const_iterator j = builders.begin(); j != builders.end(); ++j ) {
                    j->first->appendAs( forward ? fr.min() : fr.max(), "" );
                    j->second->appendAs( forward ? fr.max() : fr.min(), "" );
                }
            }
        }
        BoundList ret;
        for( BoundBuilders::const_iterator i = builders.begin(); i != builders.end(); ++i )
            ret.push_back( make_pair( i->first->obj(), i->second->obj() ) );
        return ret;
    }

    ///////////////////
    // FieldMatcher //
    ///////////////////
    
    void FieldMatcher::add( const BSONObj& o ){
        massert( 10371 , "can only add to FieldMatcher once", _source.isEmpty());
        _source = o;

        BSONObjIterator i( o );
        int true_false = -1;
        while ( i.more() ){
            BSONElement e = i.next();

            if (e.type() == Object){
                BSONObj obj = e.embeddedObject();
                BSONElement e2 = obj.firstElement();
                if ( strcmp(e2.fieldName(), "$slice") == 0 ){
                    if (e2.isNumber()){
                        int i = e2.numberInt();
                        if (i < 0)
                            add(e.fieldName(), i, -i); // limit is now positive
                        else
                            add(e.fieldName(), 0, i);

                    } else if (e2.type() == Array) {
                        BSONObj arr = e2.embeddedObject();
                        uassert(13099, "$slice array wrong size", arr.nFields() == 2 );

                        BSONObjIterator it(arr);
                        int skip = it.next().numberInt();
                        int limit = it.next().numberInt();
                        uassert(13100, "$slice limit must be positive", limit > 0 );
                        add(e.fieldName(), skip, limit);

                    } else {
                        uassert(13098, "$slice only supports numbers and [skip, limit] arrays", false);
                    }
                } else {
                    uassert(13097, string("Unsupported projection option: ") + obj.firstElement().fieldName(), false);
                }

            } else if (!strcmp(e.fieldName(), "_id") && !e.trueValue()){
                _includeID = false;

            } else {

                add (e.fieldName(), e.trueValue());

                // validate input
                if (true_false == -1){
                    true_false = e.trueValue();
                    _include = !e.trueValue();
                }
                else{
                    uassert( 10053 , "You cannot currently mix including and excluding fields. Contact us if this is an issue." , 
                             (bool)true_false == e.trueValue() );
                }
            }
        }
    }

    void FieldMatcher::add(const string& field, bool include){
        if (field.empty()){ // this is the field the user referred to
            _include = include;
        } else {
            _include = !include;

            const size_t dot = field.find('.');
            const string subfield = field.substr(0,dot);
            const string rest = (dot == string::npos ? "" : field.substr(dot+1,string::npos)); 

            boost::shared_ptr<FieldMatcher>& fm = _fields[subfield];
            if (!fm)
                fm.reset(new FieldMatcher());

            fm->add(rest, include);
        }
    }

    void FieldMatcher::add(const string& field, int skip, int limit){
        _special = true; // can't include or exclude whole object

        if (field.empty()){ // this is the field the user referred to
            _skip = skip;
            _limit = limit;
        } else {
            const size_t dot = field.find('.');
            const string subfield = field.substr(0,dot);
            const string rest = (dot == string::npos ? "" : field.substr(dot+1,string::npos));

            boost::shared_ptr<FieldMatcher>& fm = _fields[subfield];
            if (!fm)
                fm.reset(new FieldMatcher());

            fm->add(rest, skip, limit);
        }
    }

    BSONObj FieldMatcher::getSpec() const{
        return _source;
    }

    //b will be the value part of an array-typed BSONElement
    void FieldMatcher::appendArray( BSONObjBuilder& b , const BSONObj& a , bool nested) const {
        int skip  = nested ?  0 : _skip;
        int limit = nested ? -1 : _limit;

        if (skip < 0){
            skip = max(0, skip + a.nFields());
        }

        int i=0;
        BSONObjIterator it(a);
        while (it.more()){
            BSONElement e = it.next();

            if (skip){
                skip--;
                continue;
            }

            if (limit != -1 && (limit-- == 0)){
                break;
            }

            switch(e.type()){
                case Array:{
                    BSONObjBuilder subb;
                    appendArray(subb , e.embeddedObject(), true);
                    b.appendArray(b.numStr(i++).c_str(), subb.obj());
                    break;
                }
                case Object:{
                    BSONObjBuilder subb;
                    BSONObjIterator jt(e.embeddedObject());
                    while (jt.more()){
                        append(subb , jt.next());
                    }
                    b.append(b.numStr(i++), subb.obj());
                    break;
                }
                default:
                    if (_include)
                        b.appendAs(e, b.numStr(i++).c_str());
            }
        }
    }

    void FieldMatcher::append( BSONObjBuilder& b , const BSONElement& e ) const {
        FieldMap::const_iterator field = _fields.find( e.fieldName() );
        
        if (field == _fields.end()){
            if (_include)
                b.append(e);
        } 
        else {
            FieldMatcher& subfm = *field->second;
            
            if ((subfm._fields.empty() && !subfm._special) || !(e.type()==Object || e.type()==Array) ){
                if (subfm._include)
                    b.append(e);
            }
            else if (e.type() == Object){ 
                BSONObjBuilder subb;
                BSONObjIterator it(e.embeddedObject());
                while (it.more()){
                    subfm.append(subb, it.next());
                }
                b.append(e.fieldName(), subb.obj());

            } 
            else { //Array
                BSONObjBuilder subb;
                subfm.appendArray(subb, e.embeddedObject());
                b.appendArray(e.fieldName(), subb.obj());
            }
        }
    }
    
    struct SimpleRegexUnitTest : UnitTest {
        void run(){
            {
                BSONObjBuilder b;
                b.appendRegex("r", "^foo");
                BSONObj o = b.done();
                assert( simpleRegex(o.firstElement()) == "foo" );
            }
            {
                BSONObjBuilder b;
                b.appendRegex("r", "^f?oo");
                BSONObj o = b.done();
                assert( simpleRegex(o.firstElement()) == "" );
            }
            {
                BSONObjBuilder b;
                b.appendRegex("r", "^fz?oo");
                BSONObj o = b.done();
                assert( simpleRegex(o.firstElement()) == "f" );
            }
            {
                BSONObjBuilder b;
                b.appendRegex("r", "^f", "");
                BSONObj o = b.done();
                assert( simpleRegex(o.firstElement()) == "f" );
            }
            {
                BSONObjBuilder b;
                b.appendRegex("r", "\\Af", "");
                BSONObj o = b.done();
                assert( simpleRegex(o.firstElement()) == "f" );
            }
            {
                BSONObjBuilder b;
                b.appendRegex("r", "^f", "m");
                BSONObj o = b.done();
                assert( simpleRegex(o.firstElement()) == "" );
            }
            {
                BSONObjBuilder b;
                b.appendRegex("r", "\\Af", "m");
                BSONObj o = b.done();
                assert( simpleRegex(o.firstElement()) == "f" );
            }
            {
                BSONObjBuilder b;
                b.appendRegex("r", "\\Af", "mi");
                BSONObj o = b.done();
                assert( simpleRegex(o.firstElement()) == "" );
            }
            {
                BSONObjBuilder b;
                b.appendRegex("r", "\\Af \t\vo\n\ro  \\ \\# #comment", "mx");
                BSONObj o = b.done();
                assert( simpleRegex(o.firstElement()) == "foo #" );
            }
        }
    } simple_regex_unittest;
} // namespace mongo
