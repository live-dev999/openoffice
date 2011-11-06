/**************************************************************
 * 
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 * 
 *   http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 * 
 *************************************************************/



#include "oox/core/contexthandler.hxx"

#include "oox/core/fragmenthandler.hxx"

namespace oox {
namespace core {

// ============================================================================

using namespace ::com::sun::star::uno;
using namespace ::com::sun::star::xml::sax;

using ::rtl::OUString;

// ============================================================================

ContextHandler::ContextHandler( ContextHandler& rParent ) :
    ContextHandler_BASE(),
    mxBaseData( rParent.mxBaseData )
{
}

ContextHandler::ContextHandler( const FragmentBaseDataRef& rxBaseData ) :
    mxBaseData( rxBaseData )
{
}

ContextHandler::~ContextHandler()
{
}

XmlFilterBase& ContextHandler::getFilter() const
{
    return mxBaseData->mrFilter;
}

const Relations& ContextHandler::getRelations() const
{
    return *mxBaseData->mxRelations;
}

const OUString& ContextHandler::getFragmentPath() const
{
    return mxBaseData->maFragmentPath;
}

OUString ContextHandler::getFragmentPathFromRelation( const Relation& rRelation ) const
{
    return mxBaseData->mxRelations->getFragmentPathFromRelation( rRelation );
}

OUString ContextHandler::getFragmentPathFromRelId( const OUString& rRelId ) const
{
    return mxBaseData->mxRelations->getFragmentPathFromRelId( rRelId );
}

OUString ContextHandler::getFragmentPathFromFirstType( const OUString& rType ) const
{
    return mxBaseData->mxRelations->getFragmentPathFromFirstType( rType );
}

void ContextHandler::implSetLocator( const Reference< XLocator >& rxLocator )
{
    mxBaseData->mxLocator = rxLocator;
}

// com.sun.star.xml.sax.XFastContextHandler interface -------------------------

void ContextHandler::startFastElement( sal_Int32, const Reference< XFastAttributeList >& ) throw( SAXException, RuntimeException )
{
}

void ContextHandler::startUnknownElement( const OUString&, const OUString&, const Reference< XFastAttributeList >& ) throw( SAXException, RuntimeException )
{
}

void ContextHandler::endFastElement( sal_Int32 ) throw( SAXException, RuntimeException )
{
}

void ContextHandler::endUnknownElement( const OUString&, const OUString& ) throw( SAXException, RuntimeException )
{
}

Reference< XFastContextHandler > ContextHandler::createFastChildContext( sal_Int32, const Reference< XFastAttributeList >& ) throw( SAXException, RuntimeException )
{
    return 0;
}

Reference< XFastContextHandler > ContextHandler::createUnknownChildContext( const OUString&, const OUString&, const Reference< XFastAttributeList >& ) throw( SAXException, RuntimeException )
{
    return 0;
}

void ContextHandler::characters( const OUString& ) throw( SAXException, RuntimeException )
{
}

void ContextHandler::ignorableWhitespace( const OUString& ) throw( SAXException, RuntimeException )
{
}

void ContextHandler::processingInstruction( const OUString&, const OUString& ) throw( SAXException, RuntimeException )
{
}

// record context interface ---------------------------------------------------

ContextHandlerRef ContextHandler::createRecordContext( sal_Int32, SequenceInputStream& )
{
    return 0;
}

void ContextHandler::startRecord( sal_Int32, SequenceInputStream& )
{
}

void ContextHandler::endRecord( sal_Int32 )
{
}

// ============================================================================

} // namespace core
} // namespace oox
