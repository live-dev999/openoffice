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



// MARKER(update_precomp.py): autogen include statement, do not remove
#include "precompiled_svl.hxx"
#ifndef GCC
#endif

// #include <math.h>
#include <tools/debug.hxx>
#include <unotools/charclass.hxx>
#include <i18npool/mslangid.hxx>
#include <unotools/localedatawrapper.hxx>
#include <unotools/numberformatcodewrapper.hxx>
#include <unotools/calendarwrapper.hxx>
#include <com/sun/star/i18n/KNumberFormatUsage.hpp>
#include <com/sun/star/i18n/KNumberFormatType.hpp>
#include <comphelper/processfactory.hxx>
#include <unotools/misccfg.hxx>

#define _SVSTDARR_USHORTS
#include <svl/svstdarr.hxx>

#define _ZFORLIST_CXX
#include <osl/mutex.hxx>
#include <svl/zforlist.hxx>
#undef _ZFORLIST_CXX

#include "zforscan.hxx"
#include "zforfind.hxx"
#include <svl/zformat.hxx>
#include "numhead.hxx"

#include <unotools/syslocaleoptions.hxx>
#include <unotools/digitgroupingiterator.hxx>
#include <rtl/logfile.hxx>
#include <rtl/instance.hxx>

#include <math.h>
#include <limits>

using namespace ::com::sun::star;
using namespace ::com::sun::star::uno;
using namespace ::com::sun::star::i18n;
using namespace ::com::sun::star::lang;


// Constants for type offsets per Country/Language (CL)
#define ZF_STANDARD              0
#define ZF_STANDARD_PERCENT     10
#define ZF_STANDARD_CURRENCY    20
#define ZF_STANDARD_DATE        30
#define ZF_STANDARD_TIME        40
#define ZF_STANDARD_DATETIME    50
#define ZF_STANDARD_SCIENTIFIC  60
#define ZF_STANDARD_FRACTION    70
#define ZF_STANDARD_NEWEXTENDED	75
#define ZF_STANDARD_NEWEXTENDEDMAX	SV_MAX_ANZ_STANDARD_FORMATE-2	// 98
#define ZF_STANDARD_LOGICAL     SV_MAX_ANZ_STANDARD_FORMATE-1 //  99
#define ZF_STANDARD_TEXT        SV_MAX_ANZ_STANDARD_FORMATE   // 100

/* Locale that is set if an unknown locale (from another system) is loaded of
 * legacy documents. Can not be SYSTEM because else, for example, a German "DM"
 * (old currency) is recognized as a date (#53155#). */
#define UNKNOWN_SUBSTITUTE		LANGUAGE_ENGLISH_US

static sal_Bool bIndexTableInitialized = sal_False;
static sal_uInt32 __FAR_DATA theIndexTable[NF_INDEX_TABLE_ENTRIES];


// ====================================================================

/**
    instead of every number formatter being a listener we have a registry which
    also handles one instance of the SysLocale options
 */

class SvNumberFormatterRegistry_Impl : public utl::ConfigurationListener
{
    List                    aFormatters;
    SvtSysLocaleOptions     aSysLocaleOptions;
    LanguageType            eSysLanguage;

public:
                            SvNumberFormatterRegistry_Impl();
    virtual                 ~SvNumberFormatterRegistry_Impl();

            void            Insert( SvNumberFormatter* pThis )
                                { aFormatters.Insert( pThis, LIST_APPEND ); }
            SvNumberFormatter*  Remove( SvNumberFormatter* pThis )
                                    { return (SvNumberFormatter*)aFormatters.Remove( pThis ); }
            sal_uInt32           Count()
                                { return aFormatters.Count(); }

			virtual void ConfigurationChanged( utl::ConfigurationBroadcaster*, sal_uInt32 );
};


SvNumberFormatterRegistry_Impl::SvNumberFormatterRegistry_Impl()
{
    eSysLanguage = MsLangId::getRealLanguage( LANGUAGE_SYSTEM );
    aSysLocaleOptions.AddListener( this );
}


SvNumberFormatterRegistry_Impl::~SvNumberFormatterRegistry_Impl()
{
    aSysLocaleOptions.RemoveListener( this );
}


void SvNumberFormatterRegistry_Impl::ConfigurationChanged( utl::ConfigurationBroadcaster*, sal_uInt32 nHint )
{
        if ( nHint & SYSLOCALEOPTIONS_HINT_LOCALE )
        {
            ::osl::MutexGuard aGuard( SvNumberFormatter::GetMutex() );
            for ( SvNumberFormatter* p = (SvNumberFormatter*)aFormatters.First();
                    p; p = (SvNumberFormatter*)aFormatters.Next() )
            {
                p->ReplaceSystemCL( eSysLanguage );
            }
            eSysLanguage = MsLangId::getRealLanguage( LANGUAGE_SYSTEM );
        }
        if ( nHint & SYSLOCALEOPTIONS_HINT_CURRENCY )
        {
            ::osl::MutexGuard aGuard( SvNumberFormatter::GetMutex() );
            for ( SvNumberFormatter* p = (SvNumberFormatter*)aFormatters.First();
                    p; p = (SvNumberFormatter*)aFormatters.Next() )
            {
                p->ResetDefaultSystemCurrency();
            }
        }
}


// ====================================================================

SvNumberFormatterRegistry_Impl* SvNumberFormatter::pFormatterRegistry = NULL;
sal_Bool SvNumberFormatter::bCurrencyTableInitialized = sal_False;
namespace
{
    struct theCurrencyTable :
        public rtl::Static< NfCurrencyTable, theCurrencyTable > {};

    struct theLegacyOnlyCurrencyTable :
        public rtl::Static< NfCurrencyTable, theLegacyOnlyCurrencyTable > {};
}
sal_uInt16 SvNumberFormatter::nSystemCurrencyPosition = 0;
SV_IMPL_PTRARR( NfCurrencyTable, NfCurrencyEntry* );
SV_IMPL_PTRARR( NfWSStringsDtor, String* );

// ob das BankSymbol immer am Ende ist (1 $;-1 $) oder sprachabhaengig
#define NF_BANKSYMBOL_FIX_POSITION 1


/***********************Funktionen SvNumberFormatter**************************/

const sal_uInt16 SvNumberFormatter::UNLIMITED_PRECISION   = ::std::numeric_limits<sal_uInt16>::max();
const sal_uInt16 SvNumberFormatter::INPUTSTRING_PRECISION = ::std::numeric_limits<sal_uInt16>::max()-1;

SvNumberFormatter::SvNumberFormatter(
			const Reference< XMultiServiceFactory >& xSMgr,
			LanguageType eLang )
		:
		xServiceManager( xSMgr )
{
	ImpConstruct( eLang );
}


SvNumberFormatter::SvNumberFormatter( LanguageType eLang )
{
	ImpConstruct( eLang );
}


SvNumberFormatter::~SvNumberFormatter()
{
    {
        ::osl::MutexGuard aGuard( GetMutex() );
        pFormatterRegistry->Remove( this );
        if ( !pFormatterRegistry->Count() )
        {
            delete pFormatterRegistry;
            pFormatterRegistry = NULL;
        }
    }

	SvNumberformat* pEntry = aFTable.First();
	while (pEntry)
	{
		delete pEntry;
		pEntry = aFTable.Next();
	}
	delete pFormatTable;
	delete pCharClass;
	delete pStringScanner;
	delete pFormatScanner;
	ClearMergeTable();
	delete pMergeTable;
}


void SvNumberFormatter::ImpConstruct( LanguageType eLang )
{
    RTL_LOGFILE_CONTEXT_AUTHOR( aTimeLog, "svl", "er93726", "SvNumberFormatter::ImpConstruct" );

	if ( eLang == LANGUAGE_DONTKNOW )
		eLang = UNKNOWN_SUBSTITUTE;
    IniLnge = eLang;
	ActLnge = eLang;
	eEvalDateFormat = NF_EVALDATEFORMAT_INTL;
	nDefaultSystemCurrencyFormat = NUMBERFORMAT_ENTRY_NOT_FOUND;

	aLocale = MsLangId::convertLanguageToLocale( eLang );
	pCharClass = new CharClass( xServiceManager, aLocale );
    xLocaleData.init( xServiceManager, aLocale, eLang );
    xCalendar.init( xServiceManager, aLocale );
    xTransliteration.init( xServiceManager, eLang,
        ::com::sun::star::i18n::TransliterationModules_IGNORE_CASE );
    xNatNum.init( xServiceManager );

    // cached locale data items
    const LocaleDataWrapper* pLoc = GetLocaleData();
    aDecimalSep = pLoc->getNumDecimalSep();
    aThousandSep = pLoc->getNumThousandSep();
    aDateSep = pLoc->getDateSep();

	pStringScanner = new ImpSvNumberInputScan( this );
	pFormatScanner = new ImpSvNumberformatScan( this );
	pFormatTable = NULL;
	MaxCLOffset = 0;
    ImpGenerateFormats( 0, sal_False );     // 0 .. 999 for initialized language formats
    pMergeTable = NULL;
	bNoZero = sal_False;

    ::osl::MutexGuard aGuard( GetMutex() );
    GetFormatterRegistry().Insert( this );
}


void SvNumberFormatter::ChangeIntl(LanguageType eLnge)
{
	if (ActLnge != eLnge)
	{
		ActLnge = eLnge;

		aLocale = MsLangId::convertLanguageToLocale( eLnge );
		pCharClass->setLocale( aLocale );
        xLocaleData.changeLocale( aLocale, eLnge );
        xCalendar.changeLocale( aLocale );
        xTransliteration.changeLocale( eLnge );

        // cached locale data items, initialize BEFORE calling ChangeIntl below
        const LocaleDataWrapper* pLoc = GetLocaleData();
        aDecimalSep = pLoc->getNumDecimalSep();
        aThousandSep = pLoc->getNumThousandSep();
        aDateSep = pLoc->getDateSep();

		pFormatScanner->ChangeIntl();
		pStringScanner->ChangeIntl();
	}
}


// static
::osl::Mutex& SvNumberFormatter::GetMutex()
{
    static ::osl::Mutex* pMutex = NULL;
    if( !pMutex )
    {
        ::osl::MutexGuard aGuard( ::osl::Mutex::getGlobalMutex() );
        if( !pMutex )
        {
            // #i77768# Due to a static reference in the toolkit lib
            // we need a mutex that lives longer than the svl library.
            // Otherwise the dtor would use a destructed mutex!!
            pMutex = new ::osl::Mutex;
        }
    }
    return *pMutex;
}


// static
SvNumberFormatterRegistry_Impl& SvNumberFormatter::GetFormatterRegistry()
{
    ::osl::MutexGuard aGuard( GetMutex() );
    if ( !pFormatterRegistry )
        pFormatterRegistry = new SvNumberFormatterRegistry_Impl;
    return *pFormatterRegistry;
}


Color* SvNumberFormatter::GetUserDefColor(sal_uInt16 nIndex)
{
    if( aColorLink.IsSet() )
        return (Color*) ( aColorLink.Call( (void*) &nIndex ));
	else
		return NULL;
}

void SvNumberFormatter::ChangeNullDate(sal_uInt16 nDay,
									   sal_uInt16 nMonth,
									   sal_uInt16 nYear)
{
	pFormatScanner->ChangeNullDate(nDay, nMonth, nYear);
	pStringScanner->ChangeNullDate(nDay, nMonth, nYear);
}

Date* SvNumberFormatter::GetNullDate()
{
	return pFormatScanner->GetNullDate();
}

void SvNumberFormatter::ChangeStandardPrec(short nPrec)
{
	pFormatScanner->ChangeStandardPrec(nPrec);
}

sal_uInt16 SvNumberFormatter::GetStandardPrec()
{
	return pFormatScanner->GetStandardPrec();
}

void SvNumberFormatter::ImpChangeSysCL( LanguageType eLnge, sal_Bool bLoadingSO5 )
{
	if (eLnge == LANGUAGE_DONTKNOW)
		eLnge = UNKNOWN_SUBSTITUTE;
    if (eLnge != IniLnge)
	{
        IniLnge = eLnge;
		ChangeIntl(eLnge);
		SvNumberformat* pEntry = aFTable.First();
		while (pEntry)							// delete old formats
		{
			pEntry = (SvNumberformat*) aFTable.Remove(aFTable.GetCurKey());
			delete pEntry;
			pEntry = (SvNumberformat*) aFTable.First();
		}
		ImpGenerateFormats( 0, bLoadingSO5 );	// new standard formats
	}
	else if ( bLoadingSO5 )
	{	// delete additional standard formats
		sal_uInt32 nKey;
		aFTable.Seek( SV_MAX_ANZ_STANDARD_FORMATE + 1 );
		while ( (nKey = aFTable.GetCurKey()) > SV_MAX_ANZ_STANDARD_FORMATE &&
				nKey < SV_COUNTRY_LANGUAGE_OFFSET )
		{
			SvNumberformat* pEntry = (SvNumberformat*) aFTable.Remove( nKey );
			delete pEntry;
		}
	}
}


void SvNumberFormatter::ReplaceSystemCL( LanguageType eOldLanguage )
{
    sal_uInt32 nCLOffset = ImpGetCLOffset( LANGUAGE_SYSTEM );
    if ( nCLOffset > MaxCLOffset )
        return ;    // no SYSTEM entries to replace

    const sal_uInt32 nMaxBuiltin = nCLOffset + SV_MAX_ANZ_STANDARD_FORMATE;
    const sal_uInt32 nNextCL = nCLOffset + SV_COUNTRY_LANGUAGE_OFFSET;
    sal_uInt32 nKey;

    // remove old builtin formats
    aFTable.Seek( nCLOffset );
    while ( (nKey = aFTable.GetCurKey()) >= nCLOffset && nKey <= nMaxBuiltin && aFTable.Count() )
    {
        SvNumberformat* pEntry = (SvNumberformat*) aFTable.Remove( nKey );
        delete pEntry;
    }

    // move additional and user defined to temporary table
    Table aOldTable;
    while ( (nKey = aFTable.GetCurKey()) >= nCLOffset && nKey < nNextCL && aFTable.Count() )
    {
        SvNumberformat* pEntry = (SvNumberformat*) aFTable.Remove( nKey );
        aOldTable.Insert( nKey, pEntry );
    }

    // generate new old builtin formats
    // reset ActLnge otherwise ChangeIntl() wouldn't switch if already LANGUAGE_SYSTEM
    ActLnge = LANGUAGE_DONTKNOW;
    ChangeIntl( LANGUAGE_SYSTEM );
    ImpGenerateFormats( nCLOffset, sal_True );

    // convert additional and user defined from old system to new system
    SvNumberformat* pStdFormat = (SvNumberformat*) aFTable.Get( nCLOffset + ZF_STANDARD );
    sal_uInt32 nLastKey = nMaxBuiltin;
    pFormatScanner->SetConvertMode( eOldLanguage, LANGUAGE_SYSTEM, sal_True );
    aOldTable.First();
    while ( aOldTable.Count() )
    {
        nKey = aOldTable.GetCurKey();
        if ( nLastKey < nKey )
            nLastKey = nKey;
        SvNumberformat* pOldEntry = (SvNumberformat*) aOldTable.Remove( nKey );
        String aString( pOldEntry->GetFormatstring() );
        xub_StrLen nCheckPos = STRING_NOTFOUND;

        // Same as PutEntry() but assures key position even if format code is
        // a duplicate. Also won't mix up any LastInsertKey.
        ChangeIntl( eOldLanguage );
        LanguageType eLge = eOldLanguage;   // ConvertMode changes this
        sal_Bool bCheck = sal_False;
        SvNumberformat* pNewEntry = new SvNumberformat( aString, pFormatScanner,
            pStringScanner, nCheckPos, eLge );
        if ( nCheckPos != 0 )
            delete pNewEntry;
        else
        {
            short eCheckType = pNewEntry->GetType();
            if ( eCheckType != NUMBERFORMAT_UNDEFINED )
                pNewEntry->SetType( eCheckType | NUMBERFORMAT_DEFINED );
            else
                pNewEntry->SetType( NUMBERFORMAT_DEFINED );

            if ( !aFTable.Insert( nKey, pNewEntry ) )
                delete pNewEntry;
            else
                bCheck = sal_True;
        }
        DBG_ASSERT( bCheck, "SvNumberFormatter::ReplaceSystemCL: couldn't convert" );

        delete pOldEntry;
    }
	pFormatScanner->SetConvertMode(sal_False);
    pStdFormat->SetLastInsertKey( sal_uInt16(nLastKey - nCLOffset) );

    // append new system additional formats
	NumberFormatCodeWrapper aNumberFormatCode( xServiceManager, GetLocale() );
    ImpGenerateAdditionalFormats( nCLOffset, aNumberFormatCode, sal_True );
}


sal_Bool SvNumberFormatter::IsTextFormat(sal_uInt32 F_Index) const
{
	SvNumberformat* pFormat = (SvNumberformat*) aFTable.Get(F_Index);
	if (!pFormat)
		return sal_False;
	else
		return pFormat->IsTextFormat();
}

sal_Bool SvNumberFormatter::HasTextFormat(sal_uInt32 F_Index) const
{
	SvNumberformat* pFormat = (SvNumberformat*) aFTable.Get(F_Index);
	if (!pFormat)
		return sal_False;
	else
		return pFormat->HasTextFormat();
}

sal_Bool SvNumberFormatter::PutEntry(String& rString,
								 xub_StrLen& nCheckPos,
								 short& nType,
								 sal_uInt32& nKey,			// Formatnummer
								 LanguageType eLnge)
{
	nKey = 0;
	if (rString.Len() == 0) 							// keinen Leerstring
	{
		nCheckPos = 1;									// -> Fehler
		return sal_False;
	}
	if (eLnge == LANGUAGE_DONTKNOW)
        eLnge = IniLnge;

	ChangeIntl(eLnge);									// ggfs. austauschen
	LanguageType eLge = eLnge;                          // Umgehung const fuer ConvertMode
	sal_Bool bCheck = sal_False;
	SvNumberformat* p_Entry = new SvNumberformat(rString,
												 pFormatScanner,
												 pStringScanner,
												 nCheckPos,
												 eLge);
	if (nCheckPos == 0)							// Format ok
	{											// Typvergleich:
		short eCheckType = p_Entry->GetType();
		if ( eCheckType != NUMBERFORMAT_UNDEFINED)
		{
			p_Entry->SetType(eCheckType | NUMBERFORMAT_DEFINED);
			nType = eCheckType;
		}
		else
		{
			p_Entry->SetType(NUMBERFORMAT_DEFINED);
			nType = NUMBERFORMAT_DEFINED;
		}
		sal_uInt32 CLOffset = ImpGenerateCL(eLge);				// ggfs. neu Standard-
														// formate anlegen
		nKey = ImpIsEntry(p_Entry->GetFormatstring(),CLOffset, eLge);
		if (nKey != NUMBERFORMAT_ENTRY_NOT_FOUND)				// schon vorhanden
			delete p_Entry;
		else
		{
			SvNumberformat* pStdFormat =
					 (SvNumberformat*) aFTable.Get(CLOffset + ZF_STANDARD);
			sal_uInt32 nPos = CLOffset + pStdFormat->GetLastInsertKey();
			if (nPos - CLOffset >= SV_COUNTRY_LANGUAGE_OFFSET)
			{
				DBG_ERROR("SvNumberFormatter:: Zu viele Formate pro CL");
				delete p_Entry;
			}
			else if (!aFTable.Insert(nPos+1,p_Entry))
				delete p_Entry;
			else
			{
				bCheck = sal_True;
				nKey = nPos+1;
				pStdFormat->SetLastInsertKey((sal_uInt16) (nKey-CLOffset));
			}
		}
	}
	else
		delete p_Entry;
	return bCheck;
}

sal_Bool SvNumberFormatter::PutandConvertEntry(String& rString,
										   xub_StrLen& nCheckPos,
										   short& nType,
										   sal_uInt32& nKey,
										   LanguageType eLnge,
										   LanguageType eNewLnge)
{
	sal_Bool bRes;
	if (eNewLnge == LANGUAGE_DONTKNOW)
        eNewLnge = IniLnge;

	pFormatScanner->SetConvertMode(eLnge, eNewLnge);
	bRes = PutEntry(rString, nCheckPos, nType, nKey, eLnge);
	pFormatScanner->SetConvertMode(sal_False);
	return bRes;
}


sal_Bool SvNumberFormatter::PutandConvertEntrySystem(String& rString,
										   xub_StrLen& nCheckPos,
										   short& nType,
										   sal_uInt32& nKey,
										   LanguageType eLnge,
										   LanguageType eNewLnge)
{
	sal_Bool bRes;
	if (eNewLnge == LANGUAGE_DONTKNOW)
        eNewLnge = IniLnge;

	pFormatScanner->SetConvertMode(eLnge, eNewLnge, sal_True);
	bRes = PutEntry(rString, nCheckPos, nType, nKey, eLnge);
	pFormatScanner->SetConvertMode(sal_False);
	return bRes;
}


sal_uInt32 SvNumberFormatter::GetIndexPuttingAndConverting( String & rString,
        LanguageType eLnge, LanguageType eSysLnge, short & rType,
        sal_Bool & rNewInserted, xub_StrLen & rCheckPos )
{
    sal_uInt32 nKey = NUMBERFORMAT_ENTRY_NOT_FOUND;
    rNewInserted = sal_False;
    rCheckPos = 0;

    // #62389# empty format string (of Writer) => General standard format
    if (!rString.Len())
        ;   // nothing
	else if (eLnge == LANGUAGE_SYSTEM && eSysLnge != SvtSysLocale().GetLanguage())
    {
        sal_uInt32 nOrig = GetEntryKey( rString, eSysLnge );
        if (nOrig == NUMBERFORMAT_ENTRY_NOT_FOUND)
            nKey = nOrig;   // none available, maybe user-defined
        else
            nKey = GetFormatForLanguageIfBuiltIn( nOrig, SvtSysLocale().GetLanguage() );

        if (nKey == nOrig)
        {
            // Not a builtin format, convert.
            // The format code string may get modified and adapted to the real
            // language and wouldn't match eSysLnge anymore, do that on a copy.
            String aTmp( rString);
            rNewInserted = PutandConvertEntrySystem( aTmp, rCheckPos, rType,
                    nKey, eLnge, SvtSysLocale().GetLanguage());
            if (rCheckPos > 0)
            {
                DBG_ERRORFILE("SvNumberFormatter::GetIndexPuttingAndConverting: bad format code string for current locale");
                nKey = NUMBERFORMAT_ENTRY_NOT_FOUND;
            }
        }
    }
    else
    {
        nKey = GetEntryKey( rString, eLnge);
        if (nKey == NUMBERFORMAT_ENTRY_NOT_FOUND)
        {
            rNewInserted = PutEntry( rString, rCheckPos, rType, nKey, eLnge);
            if (rCheckPos > 0)
            {
                DBG_ERRORFILE("SvNumberFormatter::GetIndexPuttingAndConverting: bad format code string for specified locale");
                nKey = NUMBERFORMAT_ENTRY_NOT_FOUND;
            }
        }
    }
    if (nKey == NUMBERFORMAT_ENTRY_NOT_FOUND)
        nKey = GetStandardIndex( eLnge);
    rType = GetType( nKey);
    // Convert any (!) old "automatic" currency format to new fixed currency
    // default format.
    if ((rType & NUMBERFORMAT_CURRENCY) != 0)
    {
        const SvNumberformat* pFormat = GetEntry( nKey);
        if (!pFormat->HasNewCurrency())
        {
            if (rNewInserted)
            {
                DeleteEntry( nKey);     // don't leave trails of rubbish
                rNewInserted = sal_False;
            }
            nKey = GetStandardFormat( NUMBERFORMAT_CURRENCY, eLnge);
        }
    }
    return nKey;
}


void SvNumberFormatter::DeleteEntry(sal_uInt32 nKey)
{
	SvNumberformat* pEntry = aFTable.Remove(nKey);
	delete pEntry;
}

void SvNumberFormatter::PrepareSave()
{
	 SvNumberformat* pFormat = aFTable.First();
	 while (pFormat)
	 {
		pFormat->SetUsed(sal_False);
		pFormat = aFTable.Next();
	 }
}

void SvNumberFormatter::SetFormatUsed(sal_uInt32 nFIndex)
{
	SvNumberformat* pFormat = (SvNumberformat*) aFTable.Get(nFIndex);
	if (pFormat)
		pFormat->SetUsed(sal_True);
}

sal_Bool SvNumberFormatter::Load( SvStream& rStream )
{
    LanguageType eSysLang = SvtSysLocale().GetLanguage();
	SvNumberFormatter* pConverter = NULL;

	ImpSvNumMultipleReadHeader aHdr( rStream );
	sal_uInt16 nVersion;
	rStream >> nVersion;
	SvNumberformat* pEntry;
	sal_uInt32 nPos;
	LanguageType eSaveSysLang, eLoadSysLang;
	sal_uInt16 nSysOnStore, eLge, eDummy; 			// Dummy fuer kompatibles Format
	rStream >> nSysOnStore >> eLge;				// Systemeinstellung aus
												// Dokument
	eSaveSysLang = (nVersion < SV_NUMBERFORMATTER_VERSION_SYSTORE ?
		LANGUAGE_SYSTEM : (LanguageType) nSysOnStore);
	LanguageType eLnge = (LanguageType) eLge;
	ImpChangeSysCL( eLnge, sal_True );

	rStream >> nPos;
	while (nPos != NUMBERFORMAT_ENTRY_NOT_FOUND)
	{
		rStream >> eDummy >> eLge;
		eLnge = (LanguageType) eLge;
		ImpGenerateCL( eLnge, sal_True );			// ggfs. neue Standardformate anlegen

		sal_uInt32 nOffset = nPos % SV_COUNTRY_LANGUAGE_OFFSET;		// relativIndex
		sal_Bool bUserDefined = (nOffset > SV_MAX_ANZ_STANDARD_FORMATE);
		//! HACK! ER 29.07.97 15:15
		// SaveLang wurde bei SYSTEM nicht gespeichert sondern war auch SYSTEM,
		// erst ab 364i Unterscheidung möglich
		sal_Bool bConversionHack;
		if ( eLnge == LANGUAGE_SYSTEM )
		{
			if ( nVersion < SV_NUMBERFORMATTER_VERSION_SYSTORE )
			{
				bConversionHack = bUserDefined;
				eLoadSysLang = eSaveSysLang;
			}
			else
			{
				bConversionHack = sal_False;
				eLoadSysLang = eSysLang;
			}
		}
		else
		{
			bConversionHack = sal_False;
			eLoadSysLang = eSaveSysLang;
		}

		pEntry = new SvNumberformat(*pFormatScanner, eLnge);
		if ( bConversionHack )
		{	// SYSTEM
			// nVersion < SV_NUMBERFORMATTER_VERSION_SYSTORE
			// nVersion < SV_NUMBERFORMATTER_VERSION_KEYWORDS
			if ( !pConverter )
				pConverter = new SvNumberFormatter( xServiceManager, eSysLang );
			NfHackConversion eHackConversion = pEntry->Load(
				rStream, aHdr, pConverter, *pStringScanner );
			switch ( eHackConversion )
			{
				case NF_CONVERT_GERMAN_ENGLISH :
					pEntry->ConvertLanguage( *pConverter,
						LANGUAGE_ENGLISH_US, eSysLang, sal_True );
				break;
				case NF_CONVERT_ENGLISH_GERMAN :
					switch ( eSysLang )
					{
						case LANGUAGE_GERMAN:
						case LANGUAGE_GERMAN_SWISS:
						case LANGUAGE_GERMAN_AUSTRIAN:
						case LANGUAGE_GERMAN_LUXEMBOURG:
						case LANGUAGE_GERMAN_LIECHTENSTEIN:
							// alles beim alten
						break;
						default:
							pEntry->ConvertLanguage( *pConverter,
								LANGUAGE_GERMAN, eSysLang, sal_True );
					}
				break;
				case NF_CONVERT_NONE :
				break;  // -Wall not handled.
			}

		}
		else
		{
			pEntry->Load( rStream, aHdr, NULL, *pStringScanner );
			if ( !bUserDefined )
				bUserDefined = (pEntry->GetNewStandardDefined() > SV_NUMBERFORMATTER_VERSION);
			if ( bUserDefined )
			{
				if ( eSaveSysLang != eLoadSysLang )
				{	// SYSTEM verschieden
					if ( !pConverter )
						pConverter = new SvNumberFormatter( xServiceManager, eSysLang );
					if ( nVersion < SV_NUMBERFORMATTER_VERSION_KEYWORDS )
					{
						switch ( eSaveSysLang )
						{
							case LANGUAGE_GERMAN:
							case LANGUAGE_GERMAN_SWISS:
							case LANGUAGE_GERMAN_AUSTRIAN:
							case LANGUAGE_GERMAN_LUXEMBOURG:
							case LANGUAGE_GERMAN_LIECHTENSTEIN:
								// alles beim alten
								pEntry->ConvertLanguage( *pConverter,
									eSaveSysLang, eLoadSysLang, sal_True );
							break;
							default:
								// alte english nach neuem anderen
								pEntry->ConvertLanguage( *pConverter,
									LANGUAGE_ENGLISH_US, eLoadSysLang, sal_True );
						}
					}
					else
						pEntry->ConvertLanguage( *pConverter,
							eSaveSysLang, eLoadSysLang, sal_True );
				}
				else
				{	// nicht SYSTEM oder gleiches SYSTEM
					if ( nVersion < SV_NUMBERFORMATTER_VERSION_KEYWORDS )
					{
						LanguageType eLoadLang;
						sal_Bool bSystem;
						if ( eLnge == LANGUAGE_SYSTEM )
						{
							eLoadLang = eSysLang;
							bSystem = sal_True;
						}
						else
						{
							eLoadLang = eLnge;
							bSystem = sal_False;
						}
						switch ( eLoadLang )
						{
							case LANGUAGE_GERMAN:
							case LANGUAGE_GERMAN_SWISS:
							case LANGUAGE_GERMAN_AUSTRIAN:
							case LANGUAGE_GERMAN_LUXEMBOURG:
							case LANGUAGE_GERMAN_LIECHTENSTEIN:
								// alles beim alten
							break;
							default:
								// alte english nach neuem anderen
								if ( !pConverter )
									pConverter = new SvNumberFormatter( xServiceManager, eSysLang );
								pEntry->ConvertLanguage( *pConverter,
									LANGUAGE_ENGLISH_US, eLoadLang, bSystem );
						}
					}
				}
			}
		}
		if ( nOffset == 0 )		// StandardFormat
		{
			SvNumberformat* pEnt = aFTable.Get(nPos);
			if (pEnt)
				pEnt->SetLastInsertKey(pEntry->GetLastInsertKey());
		}
		if (!aFTable.Insert(nPos, pEntry))
			delete pEntry;
		rStream >> nPos;
	}

	// ab SV_NUMBERFORMATTER_VERSION_YEAR2000
	if ( nVersion >= SV_NUMBERFORMATTER_VERSION_YEAR2000 )
	{
		aHdr.StartEntry();
		if ( aHdr.BytesLeft() >= sizeof(sal_uInt16) )
		{
			sal_uInt16 nY2k;
			rStream >> nY2k;
			if ( nVersion < SV_NUMBERFORMATTER_VERSION_TWODIGITYEAR && nY2k < 100 )
				nY2k += 1901;		// war vor src513e: 29, jetzt: 1930
			SetYear2000( nY2k );
		}
		aHdr.EndEntry();
	}

	if ( pConverter )
		delete pConverter;

	// generate additional i18n standard formats for all used locales
	LanguageType eOldLanguage = ActLnge;
	NumberFormatCodeWrapper aNumberFormatCode( xServiceManager, GetLocale() );
	SvUShorts aList;
	GetUsedLanguages( aList );
	sal_uInt16 nCount = aList.Count();
	for ( sal_uInt16 j=0; j<nCount; j++ )
	{
		LanguageType eLang = aList[j];
		ChangeIntl( eLang );
		sal_uInt32 CLOffset = ImpGetCLOffset( eLang );
		ImpGenerateAdditionalFormats( CLOffset, aNumberFormatCode, sal_True );
	}
	ChangeIntl( eOldLanguage );

	if (rStream.GetError())
		return sal_False;
	else
		return sal_True;
}

sal_Bool SvNumberFormatter::Save( SvStream& rStream ) const
{
	ImpSvNumMultipleWriteHeader aHdr( rStream );
	// ab 364i wird gespeichert was SYSTEM wirklich war, vorher hart LANGUAGE_SYSTEM
	rStream << (sal_uInt16) SV_NUMBERFORMATTER_VERSION;
    rStream << (sal_uInt16) SvtSysLocale().GetLanguage() << (sal_uInt16) IniLnge;
	SvNumberFormatTable* pTable = (SvNumberFormatTable*) &aFTable;
	SvNumberformat* pEntry = (SvNumberformat*) pTable->First();
	while (pEntry)
	{
		// Gespeichert werden alle markierten, benutzerdefinierten Formate und
		// jeweils das Standardformat zu allen angewaehlten CL-Kombinationen
		// sowie NewStandardDefined
		if ( pEntry->GetUsed() || (pEntry->GetType() & NUMBERFORMAT_DEFINED) ||
				pEntry->GetNewStandardDefined() ||
				(pTable->GetCurKey() % SV_COUNTRY_LANGUAGE_OFFSET == 0) )
		{
			rStream << static_cast<sal_uInt32>(pTable->GetCurKey())
					<< (sal_uInt16) LANGUAGE_SYSTEM
					<< (sal_uInt16) pEntry->GetLanguage();
			pEntry->Save(rStream, aHdr);
		}
		pEntry = (SvNumberformat*) pTable->Next();
	}
	rStream << NUMBERFORMAT_ENTRY_NOT_FOUND;				// EndeKennung

	// ab SV_NUMBERFORMATTER_VERSION_YEAR2000
	aHdr.StartEntry();
	rStream << (sal_uInt16) GetYear2000();
	aHdr.EndEntry();

	if (rStream.GetError())
		return sal_False;
	else
		return sal_True;
}

// static
void SvNumberFormatter::SkipNumberFormatterInStream( SvStream& rStream )
{
	ImpSvNumMultipleReadHeader::Skip( rStream );
}

void SvNumberFormatter::GetUsedLanguages( SvUShorts& rList )
{
	rList.Remove( 0, rList.Count() );

	sal_uInt32 nOffset = 0;
	while (nOffset <= MaxCLOffset)
	{
		SvNumberformat* pFormat = (SvNumberformat*) aFTable.Get(nOffset);
		if (pFormat)
			rList.Insert( pFormat->GetLanguage(), rList.Count() );
		nOffset += SV_COUNTRY_LANGUAGE_OFFSET;
	}
}


void SvNumberFormatter::FillKeywordTable( NfKeywordTable& rKeywords,
        LanguageType eLang )
{
	ChangeIntl( eLang );
    const NfKeywordTable & rTable = pFormatScanner->GetKeywords();
	for ( sal_uInt16 i = 0; i < NF_KEYWORD_ENTRIES_COUNT; ++i )
    {
        rKeywords[i] = rTable[i];
    }
}


String SvNumberFormatter::GetKeyword( LanguageType eLnge, sal_uInt16 nIndex )
{
	ChangeIntl(eLnge);
    const NfKeywordTable & rTable = pFormatScanner->GetKeywords();
	if ( nIndex < NF_KEYWORD_ENTRIES_COUNT )
		return rTable[nIndex];

	DBG_ERROR("GetKeyword: invalid index");
	return String();
}


String SvNumberFormatter::GetStandardName( LanguageType eLnge )
{
    ChangeIntl( eLnge );
    return pFormatScanner->GetStandardName();
}


sal_uInt32 SvNumberFormatter::ImpGetCLOffset(LanguageType eLnge) const
{
	SvNumberformat* pFormat;
	sal_uInt32 nOffset = 0;
	while (nOffset <= MaxCLOffset)
	{
		pFormat = (SvNumberformat*) aFTable.Get(nOffset);
		if (pFormat && pFormat->GetLanguage() == eLnge)
			return nOffset;
		nOffset += SV_COUNTRY_LANGUAGE_OFFSET;
	}
	return nOffset;
}

sal_uInt32 SvNumberFormatter::ImpIsEntry(const String& rString,
									   sal_uInt32 nCLOffset,
									   LanguageType eLnge)
{
#ifndef NF_COMMENT_IN_FORMATSTRING
#error NF_COMMENT_IN_FORMATSTRING not defined (zformat.hxx)
#endif
#if NF_COMMENT_IN_FORMATSTRING
	String aStr( rString );
	SvNumberformat::EraseComment( aStr );
#endif
	sal_uInt32 res = NUMBERFORMAT_ENTRY_NOT_FOUND;
	SvNumberformat* pEntry;
	pEntry = (SvNumberformat*) aFTable.Seek(nCLOffset);
	while ( res == NUMBERFORMAT_ENTRY_NOT_FOUND &&
			pEntry && pEntry->GetLanguage() == eLnge )
	{
#if NF_COMMENT_IN_FORMATSTRING
		if ( pEntry->GetComment().Len() )
		{
			String aFormat( pEntry->GetFormatstring() );
			SvNumberformat::EraseComment( aFormat );
			if ( aStr == aFormat )
				res = aFTable.GetCurKey();
			else
				pEntry = (SvNumberformat*) aFTable.Next();
		}
		else
		{
			if ( aStr == pEntry->GetFormatstring() )
				res = aFTable.GetCurKey();
			else
				pEntry = (SvNumberformat*) aFTable.Next();
		}
#else
		if ( rString == pEntry->GetFormatstring() )
			res = aFTable.GetCurKey();
		else
			pEntry = (SvNumberformat*) aFTable.Next();
#endif
	}
	return res;
}


SvNumberFormatTable& SvNumberFormatter::GetFirstEntryTable(
													  short& eType,
													  sal_uInt32& FIndex,
													  LanguageType& rLnge)
{
	short eTypetmp = eType;
	if (eType == NUMBERFORMAT_ALL) 					// Leere Zelle oder don't care
        rLnge = IniLnge;
	else
	{
		SvNumberformat* pFormat = (SvNumberformat*) aFTable.Get(FIndex);
		if (!pFormat)
		{
//			DBG_ERROR("SvNumberFormatter:: Unbekanntes altes Zahlformat (1)");
            rLnge = IniLnge;
			eType = NUMBERFORMAT_ALL;
			eTypetmp = eType;
		}
		else
		{
			rLnge = pFormat->GetLanguage();
			eType = pFormat->GetType()&~NUMBERFORMAT_DEFINED;
			if (eType == 0)
			{
				eType = NUMBERFORMAT_DEFINED;
				eTypetmp = eType;
			}
			else if (eType == NUMBERFORMAT_DATETIME)
			{
				eTypetmp = eType;
				eType = NUMBERFORMAT_DATE;
			}
			else
				eTypetmp = eType;
		}
	}
	ChangeIntl(rLnge);
	return GetEntryTable(eTypetmp, FIndex, rLnge);
}

sal_uInt32 SvNumberFormatter::ImpGenerateCL( LanguageType eLnge, sal_Bool bLoadingSO5 )
{
	ChangeIntl(eLnge);
	sal_uInt32 CLOffset = ImpGetCLOffset(ActLnge);
	if (CLOffset > MaxCLOffset)
	{	// new CL combination
        if (LocaleDataWrapper::areChecksEnabled())
        {
            Locale aLoadedLocale = xLocaleData->getLoadedLocale();
            if ( aLoadedLocale.Language != aLocale.Language ||
                    aLoadedLocale.Country != aLocale.Country )
            {
                String aMsg( RTL_CONSTASCII_USTRINGPARAM(
                            "SvNumerFormatter::ImpGenerateCL: locales don't match:"));
                LocaleDataWrapper::outputCheckMessage(
                        xLocaleData->appendLocaleInfo( aMsg ));
            }
            // test XML locale data FormatElement entries
            {
                uno::Sequence< i18n::FormatElement > xSeq =
                    xLocaleData->getAllFormats();
                // A test for completeness of formatindex="0" ...
                // formatindex="47" is not needed here since it is done in
                // ImpGenerateFormats().

                // Test for dupes of formatindex="..."
                for ( sal_Int32 j = 0; j < xSeq.getLength(); j++ )
                {
                    sal_Int16 nIdx = xSeq[j].formatIndex;
                    String aDupes;
                    for ( sal_Int32 i = 0; i < xSeq.getLength(); i++ )
                    {
                        if ( i != j && xSeq[i].formatIndex == nIdx )
                        {
                            aDupes += String::CreateFromInt32( i );
                            aDupes += '(';
                            aDupes += String( xSeq[i].formatKey );
                            aDupes += ')';
                            aDupes += ' ';
                        }
                    }
                    if ( aDupes.Len() )
                    {
                        String aMsg( RTL_CONSTASCII_USTRINGPARAM(
                                    "XML locale data FormatElement formatindex dupe: "));
                        aMsg += String::CreateFromInt32( nIdx );
                        aMsg.AppendAscii( RTL_CONSTASCII_STRINGPARAM(
                                    "\nFormatElements: "));
                        aMsg += String::CreateFromInt32( j );
                        aMsg += '(';
                        aMsg += String( xSeq[j].formatKey );
                        aMsg += ')';
                        aMsg += ' ';
                        aMsg += aDupes;
                        LocaleDataWrapper::outputCheckMessage(
                                xLocaleData->appendLocaleInfo( aMsg ));
                    }
                }
            }
        }

		MaxCLOffset += SV_COUNTRY_LANGUAGE_OFFSET;
		ImpGenerateFormats( MaxCLOffset, bLoadingSO5 );
		CLOffset = MaxCLOffset;
	}
	return CLOffset;
}

SvNumberFormatTable& SvNumberFormatter::ChangeCL(short eType,
												 sal_uInt32& FIndex,
												 LanguageType eLnge)
{
	ImpGenerateCL(eLnge);
	return GetEntryTable(eType, FIndex, ActLnge);
}

SvNumberFormatTable& SvNumberFormatter::GetEntryTable(
													short eType,
													sal_uInt32& FIndex,
													LanguageType eLnge)
{
	if ( pFormatTable )
		pFormatTable->Clear();
	else
		pFormatTable = new SvNumberFormatTable;
	ChangeIntl(eLnge);
	sal_uInt32 CLOffset = ImpGetCLOffset(ActLnge);

    // Might generate and insert a default format for the given type
    // (e.g. currency) => has to be done before collecting formats.
    sal_uInt32 nDefaultIndex = GetStandardFormat( eType, ActLnge );

	SvNumberformat* pEntry;
	pEntry = (SvNumberformat*) aFTable.Seek(CLOffset);

	if (eType == NUMBERFORMAT_ALL)
	{
		while (pEntry && pEntry->GetLanguage() == ActLnge)
        {   // copy all entries to output table
            pFormatTable->Insert( aFTable.GetCurKey(), pEntry );
			pEntry = (SvNumberformat*) aFTable.Next();
		}
	}
	else
	{
		while (pEntry && pEntry->GetLanguage() == ActLnge)
        {   // copy entries of queried type to output table
            if ((pEntry->GetType()) & eType)
                pFormatTable->Insert(aFTable.GetCurKey(),pEntry);
			pEntry = (SvNumberformat*) aFTable.Next();
		}
	}
    if ( pFormatTable->Count() > 0 )
    {   // select default if queried format doesn't exist or queried type or
        // language differ from existing format
        pEntry = aFTable.Get(FIndex);
        if ( !pEntry || !(pEntry->GetType() & eType) || pEntry->GetLanguage() != ActLnge )
            FIndex = nDefaultIndex;
    }
	return *pFormatTable;
}

sal_Bool SvNumberFormatter::IsNumberFormat(const String& sString,
									   sal_uInt32& F_Index,
									   double& fOutNumber)
{
	short FType;
	const SvNumberformat* pFormat = (SvNumberformat*) aFTable.Get(F_Index);
	if (!pFormat)
	{
//		DBG_ERROR("SvNumberFormatter:: Unbekanntes altes Zahlformat (2)");
        ChangeIntl(IniLnge);
		FType = NUMBERFORMAT_NUMBER;
	}
	else
	{
		FType = pFormat->GetType() &~NUMBERFORMAT_DEFINED;
		if (FType == 0)
			FType = NUMBERFORMAT_DEFINED;
		ChangeIntl(pFormat->GetLanguage());
	}
	sal_Bool res;
	short RType = FType;
														// Ergebnistyp
														// ohne def-Kennung
	if (RType == NUMBERFORMAT_TEXT)							// Zahlzelle ->Stringz.
		res = sal_False;
	else
		res = pStringScanner->IsNumberFormat(sString, RType, fOutNumber, pFormat);

	if (res && !IsCompatible(FType, RType))		// unpassender Typ
	{
		switch ( RType )
		{
			case NUMBERFORMAT_TIME :
			{
				if ( pStringScanner->GetDecPos() )
				{	// 100stel Sekunden
					if ( pStringScanner->GetAnzNums() > 3 || fOutNumber < 0.0 )
						F_Index = GetFormatIndex( NF_TIME_HH_MMSS00, ActLnge );
					else
						F_Index = GetFormatIndex( NF_TIME_MMSS00, ActLnge );
				}
				else if ( fOutNumber >= 1.0 || fOutNumber < 0.0 )
					F_Index = GetFormatIndex( NF_TIME_HH_MMSS, ActLnge );
				else
					F_Index = GetStandardFormat( RType, ActLnge );
			}
			break;
			default:
				F_Index = GetStandardFormat( RType, ActLnge );
		}
	}
	return res;
}

sal_Bool SvNumberFormatter::IsCompatible(short eOldType,
									 short eNewType)
{
	if (eOldType == eNewType)
		return sal_True;
	else if (eOldType == NUMBERFORMAT_DEFINED)
		return sal_True;
	else
	{
		switch (eNewType)
		{
			case NUMBERFORMAT_NUMBER:
			{
				switch (eOldType)
				{
					case NUMBERFORMAT_PERCENT:
					case NUMBERFORMAT_CURRENCY:
					case NUMBERFORMAT_SCIENTIFIC:
					case NUMBERFORMAT_FRACTION:
//					case NUMBERFORMAT_LOGICAL:
					case NUMBERFORMAT_DEFINED:
						return sal_True;
					default:
						return sal_False;
				}
			}
			break;
			case NUMBERFORMAT_DATE:
			{
				switch (eOldType)
				{
					case NUMBERFORMAT_DATETIME:
						return sal_True;
					default:
						return sal_False;
				}
			}
			break;
			case NUMBERFORMAT_TIME:
			{
				switch (eOldType)
				{
					case NUMBERFORMAT_DATETIME:
						return sal_True;
					default:
						return sal_False;
				}
			}
			break;
			case NUMBERFORMAT_DATETIME:
			{
				switch (eOldType)
				{
					case NUMBERFORMAT_TIME:
					case NUMBERFORMAT_DATE:
						return sal_True;
					default:
						return sal_False;
				}
			}
			break;
			default:
			return sal_False;
		}
		return sal_False;
	}
}


sal_uInt32 SvNumberFormatter::ImpGetDefaultFormat( short nType )
{
	sal_uInt32 CLOffset = ImpGetCLOffset( ActLnge );
	sal_uInt32 nSearch;
	switch( nType )
	{
		case NUMBERFORMAT_DATE		:
			nSearch = CLOffset + ZF_STANDARD_DATE;
		break;
		case NUMBERFORMAT_TIME      :
			nSearch = CLOffset + ZF_STANDARD_TIME;
		break;
		case NUMBERFORMAT_DATETIME  :
			nSearch = CLOffset + ZF_STANDARD_DATETIME;
		break;
		case NUMBERFORMAT_PERCENT   :
			nSearch = CLOffset + ZF_STANDARD_PERCENT;
		break;
		case NUMBERFORMAT_SCIENTIFIC:
			nSearch = CLOffset + ZF_STANDARD_SCIENTIFIC;
		break;
		default:
			nSearch = CLOffset + ZF_STANDARD;
	}
	sal_uInt32 nDefaultFormat = (sal_uInt32)(sal_uLong) aDefaultFormatKeys.Get( nSearch );
	if ( !nDefaultFormat )
		nDefaultFormat = NUMBERFORMAT_ENTRY_NOT_FOUND;
	if ( nDefaultFormat == NUMBERFORMAT_ENTRY_NOT_FOUND )
	{	// look for a defined standard
		sal_uInt32 nStopKey = CLOffset + SV_COUNTRY_LANGUAGE_OFFSET;
		sal_uInt32 nKey;
		aFTable.Seek( CLOffset );
		while ( (nKey = aFTable.GetCurKey()) >= CLOffset && nKey < nStopKey )
		{
			const SvNumberformat* pEntry =
				(const SvNumberformat*) aFTable.GetCurObject();
            if ( pEntry->IsStandard() && ((pEntry->GetType() &
                            ~NUMBERFORMAT_DEFINED) == nType) )
			{
				nDefaultFormat = nKey;
				break;	// while
			}
			aFTable.Next();
		}

		if ( nDefaultFormat == NUMBERFORMAT_ENTRY_NOT_FOUND )
		{	// none found, use old fixed standards
			switch( nType )
			{
				case NUMBERFORMAT_DATE		:
					nDefaultFormat = CLOffset + ZF_STANDARD_DATE;
				break;
				case NUMBERFORMAT_TIME      :
					nDefaultFormat = CLOffset + ZF_STANDARD_TIME+1;
				break;
				case NUMBERFORMAT_DATETIME  :
					nDefaultFormat = CLOffset + ZF_STANDARD_DATETIME;
				break;
				case NUMBERFORMAT_PERCENT   :
					nDefaultFormat = CLOffset + ZF_STANDARD_PERCENT+1;
				break;
				case NUMBERFORMAT_SCIENTIFIC:
					nDefaultFormat = CLOffset + ZF_STANDARD_SCIENTIFIC;
				break;
				default:
					nDefaultFormat = CLOffset + ZF_STANDARD;
			}
		}
		aDefaultFormatKeys.Insert( nSearch, (void*) nDefaultFormat );
	}
	return nDefaultFormat;
}


sal_uInt32 SvNumberFormatter::GetStandardFormat( short eType, LanguageType eLnge )
{
	sal_uInt32 CLOffset = ImpGenerateCL(eLnge);
	switch(eType)
	{
		case NUMBERFORMAT_CURRENCY  :
		{
			if ( eLnge == LANGUAGE_SYSTEM )
				return ImpGetDefaultSystemCurrencyFormat();
			else
				return ImpGetDefaultCurrencyFormat();
		}
		case NUMBERFORMAT_DATE		:
		case NUMBERFORMAT_TIME      :
		case NUMBERFORMAT_DATETIME  :
		case NUMBERFORMAT_PERCENT   :
		case NUMBERFORMAT_SCIENTIFIC:
			return ImpGetDefaultFormat( eType );

		case NUMBERFORMAT_FRACTION  : return CLOffset + ZF_STANDARD_FRACTION;
		case NUMBERFORMAT_LOGICAL   : return CLOffset + ZF_STANDARD_LOGICAL;
		case NUMBERFORMAT_TEXT		: return CLOffset + ZF_STANDARD_TEXT;
		case NUMBERFORMAT_ALL       :
		case NUMBERFORMAT_DEFINED   :
		case NUMBERFORMAT_NUMBER    :
		case NUMBERFORMAT_UNDEFINED :
		default               : return CLOffset + ZF_STANDARD;
	}
}

sal_Bool SvNumberFormatter::IsSpecialStandardFormat( sal_uInt32 nFIndex,
		LanguageType eLnge )
{
	return
		nFIndex == GetFormatIndex( NF_TIME_MMSS00, eLnge ) ||
		nFIndex == GetFormatIndex( NF_TIME_HH_MMSS00, eLnge ) ||
		nFIndex == GetFormatIndex( NF_TIME_HH_MMSS, eLnge )
		;
}

sal_uInt32 SvNumberFormatter::GetStandardFormat( sal_uInt32 nFIndex, short eType,
		LanguageType eLnge )
{
	if ( IsSpecialStandardFormat( nFIndex, eLnge ) )
		return nFIndex;
	else
		return GetStandardFormat( eType, eLnge );
}

sal_uInt32 SvNumberFormatter::GetStandardFormat( double fNumber, sal_uInt32 nFIndex,
		short eType, LanguageType eLnge )
{
	if ( IsSpecialStandardFormat( nFIndex, eLnge ) )
		return nFIndex;

	switch( eType )
	{
		case NUMBERFORMAT_TIME :
		{
			sal_Bool bSign;
			if ( fNumber < 0.0 )
			{
				bSign = sal_True;
				fNumber = -fNumber;
			}
			else
				bSign = sal_False;
			double fSeconds = fNumber * 86400;
			if ( floor( fSeconds + 0.5 ) * 100 != floor( fSeconds * 100 + 0.5 ) )
			{	// mit 100stel Sekunden
				if ( bSign || fSeconds >= 3600 )
					return GetFormatIndex( NF_TIME_HH_MMSS00, eLnge );
				else
					return GetFormatIndex( NF_TIME_MMSS00, eLnge );
			}
			else
			{
				if ( bSign || fNumber >= 1.0 )
					return GetFormatIndex( NF_TIME_HH_MMSS, eLnge );
				else
					return GetStandardFormat( eType, eLnge );
			}
		}
		default:
			return GetStandardFormat( eType, eLnge );
	}
}

void SvNumberFormatter::GetInputLineString(const double& fOutNumber,
										   sal_uInt32 nFIndex,
										   String& sOutString)
{
	SvNumberformat* pFormat;
	Color* pColor;
	pFormat = (SvNumberformat*) aFTable.Get(nFIndex);
	if (!pFormat)
		pFormat = aFTable.Get(ZF_STANDARD);
	LanguageType eLang = pFormat->GetLanguage();
	ChangeIntl( eLang );
	short eType = pFormat->GetType() & ~NUMBERFORMAT_DEFINED;
	if (eType == 0)
		eType = NUMBERFORMAT_DEFINED;
    sal_uInt16 nOldPrec = pFormatScanner->GetStandardPrec();
    bool bPrecChanged = false;
	if (eType == NUMBERFORMAT_NUMBER || eType == NUMBERFORMAT_PERCENT
									 || eType == NUMBERFORMAT_CURRENCY
									 || eType == NUMBERFORMAT_SCIENTIFIC
									 || eType == NUMBERFORMAT_FRACTION)
	{
		if (eType != NUMBERFORMAT_PERCENT)	// später Sonderbehandlung %
			eType = NUMBERFORMAT_NUMBER;
        ChangeStandardPrec(INPUTSTRING_PRECISION);
        bPrecChanged = true;
	}
	sal_uInt32 nKey = nFIndex;
	switch ( eType )
	{	// #61619# immer vierstelliges Jahr editieren
		case NUMBERFORMAT_DATE :
			nKey = GetFormatIndex( NF_DATE_SYS_DDMMYYYY, eLang );
		break;
		case NUMBERFORMAT_DATETIME :
			nKey = GetFormatIndex( NF_DATETIME_SYS_DDMMYYYY_HHMMSS, eLang );
		break;
		default:
			nKey = GetStandardFormat( fOutNumber, nFIndex, eType, eLang );
	}
	if ( nKey != nFIndex )
		pFormat = (SvNumberformat*) aFTable.Get( nKey );
	if (pFormat)
	{
		if ( eType == NUMBERFORMAT_TIME && pFormat->GetFormatPrecision() )
		{
            ChangeStandardPrec(INPUTSTRING_PRECISION);
            bPrecChanged = true;
		}
		pFormat->GetOutputString(fOutNumber, sOutString, &pColor);
	}
    if (bPrecChanged)
		ChangeStandardPrec(nOldPrec);
}

void SvNumberFormatter::GetOutputString(const double& fOutNumber,
										sal_uInt32 nFIndex,
										String& sOutString,
										Color** ppColor)
{
	if (bNoZero && fOutNumber == 0.0)
	{
		sOutString.Erase();
		return;
	}
	SvNumberformat* pFormat = (SvNumberformat*) aFTable.Get(nFIndex);
	if (!pFormat)
		pFormat = aFTable.Get(ZF_STANDARD);
	ChangeIntl(pFormat->GetLanguage());
	pFormat->GetOutputString(fOutNumber, sOutString, ppColor);
}

void SvNumberFormatter::GetOutputString(String& sString,
										sal_uInt32 nFIndex,
										String& sOutString,
										Color** ppColor)
{
	SvNumberformat* pFormat = (SvNumberformat*) aFTable.Get(nFIndex);
	if (!pFormat)
		pFormat = aFTable.Get(ZF_STANDARD_TEXT);
	if (!pFormat->IsTextFormat() && !pFormat->HasTextFormat())
	{
		*ppColor = NULL;
		sOutString = sString;
	}
	else
	{
		ChangeIntl(pFormat->GetLanguage());
		pFormat->GetOutputString(sString, sOutString, ppColor);
	}
}

sal_Bool SvNumberFormatter::GetPreviewString(const String& sFormatString,
										 double fPreviewNumber,
										 String& sOutString,
										 Color** ppColor,
										 LanguageType eLnge)
{
	if (sFormatString.Len() == 0) 						// keinen Leerstring
		return sal_False;

	xub_StrLen nCheckPos = STRING_NOTFOUND;
	sal_uInt32 nKey;
	if (eLnge == LANGUAGE_DONTKNOW)
        eLnge = IniLnge;
	ChangeIntl(eLnge);							// ggfs. austauschen
	eLnge = ActLnge;
	String sTmpString = sFormatString;
	SvNumberformat* p_Entry = new SvNumberformat(sTmpString,
												 pFormatScanner,
												 pStringScanner,
												 nCheckPos,
												 eLnge);
	if (nCheckPos == 0)									// String ok
	{
		sal_uInt32 CLOffset = ImpGenerateCL(eLnge);				// ggfs. neu Standard-
														// formate anlegen
		nKey = ImpIsEntry(p_Entry->GetFormatstring(),CLOffset, eLnge);
		if (nKey != NUMBERFORMAT_ENTRY_NOT_FOUND)				// schon vorhanden
			GetOutputString(fPreviewNumber,nKey,sOutString,ppColor);
		else
			p_Entry->GetOutputString(fPreviewNumber,sOutString, ppColor);
		delete p_Entry;
		return sal_True;
	}
	else
	{
		delete p_Entry;
		return sal_False;
	}
}

sal_Bool SvNumberFormatter::GetPreviewStringGuess( const String& sFormatString,
										 double fPreviewNumber,
										 String& sOutString,
										 Color** ppColor,
										 LanguageType eLnge )
{
	if (sFormatString.Len() == 0) 						// keinen Leerstring
		return sal_False;

	if (eLnge == LANGUAGE_DONTKNOW)
        eLnge = IniLnge;

	ChangeIntl( eLnge );
	eLnge = ActLnge;
	sal_Bool bEnglish = (eLnge == LANGUAGE_ENGLISH_US);

	String aFormatStringUpper( pCharClass->upper( sFormatString ) );
	sal_uInt32 nCLOffset = ImpGenerateCL( eLnge );
	sal_uInt32 nKey = ImpIsEntry( aFormatStringUpper, nCLOffset, eLnge );
	if ( nKey != NUMBERFORMAT_ENTRY_NOT_FOUND )
	{	// Zielformat vorhanden
		GetOutputString( fPreviewNumber, nKey, sOutString, ppColor );
		return sal_True;
	}

	SvNumberformat *pEntry = NULL;
	xub_StrLen nCheckPos = STRING_NOTFOUND;
	String sTmpString;

	if ( bEnglish )
	{
		sTmpString = sFormatString;
		pEntry = new SvNumberformat( sTmpString, pFormatScanner,
			pStringScanner, nCheckPos, eLnge );
	}
	else
	{
		nCLOffset = ImpGenerateCL( LANGUAGE_ENGLISH_US );
		nKey = ImpIsEntry( aFormatStringUpper, nCLOffset, LANGUAGE_ENGLISH_US );
		sal_Bool bEnglishFormat = (nKey != NUMBERFORMAT_ENTRY_NOT_FOUND);

		// try english --> other bzw. english nach other konvertieren
		LanguageType eFormatLang = LANGUAGE_ENGLISH_US;
		pFormatScanner->SetConvertMode( LANGUAGE_ENGLISH_US, eLnge );
		sTmpString = sFormatString;
		pEntry = new SvNumberformat( sTmpString, pFormatScanner,
			pStringScanner, nCheckPos, eFormatLang );
		pFormatScanner->SetConvertMode( sal_False );
		ChangeIntl( eLnge );

		if ( !bEnglishFormat )
		{
            if ( nCheckPos > 0 || xTransliteration->isEqual( sFormatString,
                    pEntry->GetFormatstring() ) )
			{	// other Format
				delete pEntry;
				sTmpString = sFormatString;
				pEntry = new SvNumberformat( sTmpString, pFormatScanner,
					pStringScanner, nCheckPos, eLnge );
			}
			else
			{	// verify english
				xub_StrLen nCheckPos2 = STRING_NOTFOUND;
				// try other --> english
				eFormatLang = eLnge;
				pFormatScanner->SetConvertMode( eLnge, LANGUAGE_ENGLISH_US );
				sTmpString = sFormatString;
				SvNumberformat* pEntry2 = new SvNumberformat( sTmpString, pFormatScanner,
					pStringScanner, nCheckPos2, eFormatLang );
				pFormatScanner->SetConvertMode( sal_False );
				ChangeIntl( eLnge );
                if ( nCheckPos2 == 0 && !xTransliteration->isEqual( sFormatString,
                        pEntry2->GetFormatstring() ) )
				{	// other Format
					delete pEntry;
					sTmpString = sFormatString;
					pEntry = new SvNumberformat( sTmpString, pFormatScanner,
						pStringScanner, nCheckPos, eLnge );
				}
				delete pEntry2;
			}
		}
	}

	if (nCheckPos == 0)									// String ok
	{
		ImpGenerateCL( eLnge );		// ggfs. neu Standardformate anlegen
		pEntry->GetOutputString( fPreviewNumber, sOutString, ppColor );
		delete pEntry;
		return sal_True;
	}
	delete pEntry;
	return sal_False;
}

sal_Bool SvNumberFormatter::GetPreviewString( const String& sFormatString,
                                          const String& sPreviewString,
                                          String& sOutString,
                                          Color** ppColor,
                                          LanguageType eLnge )
{
    if (sFormatString.Len() == 0)               // no empty string
        return sal_False;

    xub_StrLen nCheckPos = STRING_NOTFOUND;
    sal_uInt32 nKey;
    if (eLnge == LANGUAGE_DONTKNOW)
        eLnge = IniLnge;
    ChangeIntl(eLnge);                          // switch if needed
    eLnge = ActLnge;
    String sTmpString = sFormatString;
    SvNumberformat* p_Entry = new SvNumberformat( sTmpString,
                                                  pFormatScanner,
                                                  pStringScanner,
                                                  nCheckPos,
                                                  eLnge);
    if (nCheckPos == 0)                          // String ok
    {
        String aNonConstPreview( sPreviewString);
        // May have to create standard formats for this locale.
        sal_uInt32 CLOffset = ImpGenerateCL(eLnge);
        nKey = ImpIsEntry( p_Entry->GetFormatstring(), CLOffset, eLnge);
        if (nKey != NUMBERFORMAT_ENTRY_NOT_FOUND)       // already present
            GetOutputString( aNonConstPreview, nKey, sOutString, ppColor);
        else
        {
            // If the format is valid but not a text format and does not
            // include a text subformat, an empty string would result. Same as
            // in SvNumberFormatter::GetOutputString()
            if (p_Entry->IsTextFormat() || p_Entry->HasTextFormat())
                p_Entry->GetOutputString( aNonConstPreview, sOutString, ppColor);
            else
            {
                *ppColor = NULL;
                sOutString = sPreviewString;
            }
        }
        delete p_Entry;
        return sal_True;
    }
    else
    {
        delete p_Entry;
        return sal_False;
    }
}

sal_uInt32 SvNumberFormatter::TestNewString(const String& sFormatString,
									  LanguageType eLnge)
{
	if (sFormatString.Len() == 0) 						// keinen Leerstring
		return NUMBERFORMAT_ENTRY_NOT_FOUND;

	xub_StrLen nCheckPos = STRING_NOTFOUND;
	if (eLnge == LANGUAGE_DONTKNOW)
        eLnge = IniLnge;
	ChangeIntl(eLnge);									// ggfs. austauschen
	eLnge = ActLnge;
	sal_uInt32 nRes;
	String sTmpString = sFormatString;
	SvNumberformat* pEntry = new SvNumberformat(sTmpString,
												pFormatScanner,
												pStringScanner,
												nCheckPos,
												eLnge);
	if (nCheckPos == 0)									// String ok
	{
		sal_uInt32 CLOffset = ImpGenerateCL(eLnge);				// ggfs. neu Standard-
														// formate anlegen
		nRes = ImpIsEntry(pEntry->GetFormatstring(),CLOffset, eLnge);
														// schon vorhanden ?
	}
	else
		nRes = NUMBERFORMAT_ENTRY_NOT_FOUND;
	delete pEntry;
	return nRes;
}

SvNumberformat* SvNumberFormatter::ImpInsertFormat(
			const ::com::sun::star::i18n::NumberFormatCode& rCode,
			sal_uInt32 nPos, sal_Bool bAfterLoadingSO5, sal_Int16 nOrgIndex )
{
	String aCodeStr( rCode.Code );
	if ( rCode.Index < NF_INDEX_TABLE_ENTRIES &&
			rCode.Usage == ::com::sun::star::i18n::KNumberFormatUsage::CURRENCY &&
			rCode.Index != NF_CURRENCY_1000DEC2_CCC )
	{	// strip surrounding [$...] on automatic currency
		if ( aCodeStr.SearchAscii( "[$" ) != STRING_NOTFOUND )
			aCodeStr = SvNumberformat::StripNewCurrencyDelimiters( aCodeStr, sal_False );
		else
		{
			if (LocaleDataWrapper::areChecksEnabled() &&
                    rCode.Index != NF_CURRENCY_1000DEC2_CCC )
			{
				String aMsg( RTL_CONSTASCII_USTRINGPARAM(
                            "SvNumberFormatter::ImpInsertFormat: no [$...] on currency format code, index "));
				aMsg += String::CreateFromInt32( rCode.Index );
				aMsg.AppendAscii( RTL_CONSTASCII_STRINGPARAM( ":\n"));
				aMsg += String( rCode.Code );
                LocaleDataWrapper::outputCheckMessage(
                        xLocaleData->appendLocaleInfo( aMsg));
			}
		}
	}
	xub_StrLen nCheckPos = 0;
	SvNumberformat* pFormat = new SvNumberformat(aCodeStr,
												 pFormatScanner,
												 pStringScanner,
												 nCheckPos,
												 ActLnge);
	if ( !pFormat || nCheckPos > 0 )
	{
        if (LocaleDataWrapper::areChecksEnabled())
        {
            String aMsg( RTL_CONSTASCII_USTRINGPARAM(
                        "SvNumberFormatter::ImpInsertFormat: bad format code, index "));
            aMsg += String::CreateFromInt32( rCode.Index );
            aMsg += '\n';
            aMsg += String( rCode.Code );
            LocaleDataWrapper::outputCheckMessage(
                    xLocaleData->appendLocaleInfo( aMsg));
        }
		delete pFormat;
		return NULL;
	}
	if ( rCode.Index >= NF_INDEX_TABLE_ENTRIES )
	{
		sal_uInt32 nCLOffset = nPos - (nPos % SV_COUNTRY_LANGUAGE_OFFSET);
		sal_uInt32 nKey = ImpIsEntry( aCodeStr, nCLOffset, ActLnge );
		if ( nKey != NUMBERFORMAT_ENTRY_NOT_FOUND )
		{
            if (LocaleDataWrapper::areChecksEnabled())
            {
                switch ( nOrgIndex )
                {
                    // These may be dupes of integer versions for locales where
                    // currencies have no decimals like Italian Lira.
                    case NF_CURRENCY_1000DEC2 :			// NF_CURRENCY_1000INT
                    case NF_CURRENCY_1000DEC2_RED :		// NF_CURRENCY_1000INT_RED
                    case NF_CURRENCY_1000DEC2_DASHED :	// NF_CURRENCY_1000INT_RED
                    break;
                    default:
                        if ( !bAfterLoadingSO5 )
                        {	// If bAfterLoadingSO5 there will definitely be some dupes,
                            // don't cry. But we need this test for verification of locale
                            // data if not loading old SO5 documents.
                            String aMsg( RTL_CONSTASCII_USTRINGPARAM(
                                        "SvNumberFormatter::ImpInsertFormat: dup format code, index "));
                            aMsg += String::CreateFromInt32( rCode.Index );
                            aMsg += '\n';
                            aMsg += String( rCode.Code );
                            LocaleDataWrapper::outputCheckMessage(
                                    xLocaleData->appendLocaleInfo( aMsg));
                        }
                }
            }
			delete pFormat;
			return NULL;
		}
		else if ( nPos - nCLOffset >= SV_COUNTRY_LANGUAGE_OFFSET )
		{
            if (LocaleDataWrapper::areChecksEnabled())
            {
                String aMsg( RTL_CONSTASCII_USTRINGPARAM(
                            "SvNumberFormatter::ImpInsertFormat: too many format codes, index "));
                aMsg += String::CreateFromInt32( rCode.Index );
                aMsg += '\n';
                aMsg += String( rCode.Code );
                LocaleDataWrapper::outputCheckMessage(
                        xLocaleData->appendLocaleInfo( aMsg));
            }
			delete pFormat;
			return NULL;
		}
	}
	if ( !aFTable.Insert( nPos, pFormat ) )
	{
        if (LocaleDataWrapper::areChecksEnabled())
        {
            String aMsg( RTL_CONSTASCII_USTRINGPARAM(
                        "ImpInsertFormat: can't insert number format key pos: "));
            aMsg += String::CreateFromInt32( nPos );
            aMsg.AppendAscii( RTL_CONSTASCII_STRINGPARAM( ", code index "));
            aMsg += String::CreateFromInt32( rCode.Index );
            aMsg += '\n';
            aMsg += String( rCode.Code );
            LocaleDataWrapper::outputCheckMessage(
                    xLocaleData->appendLocaleInfo( aMsg));
        }
		delete pFormat;
		return NULL;
	}
	if ( rCode.Default )
		pFormat->SetStandard();
	if ( rCode.DefaultName.getLength() )
		pFormat->SetComment( rCode.DefaultName );
	return pFormat;
}

SvNumberformat* SvNumberFormatter::ImpInsertNewStandardFormat(
			const ::com::sun::star::i18n::NumberFormatCode& rCode,
			sal_uInt32 nPos, sal_uInt16 nVersion, sal_Bool bAfterLoadingSO5,
			sal_Int16 nOrgIndex )
{
	SvNumberformat* pNewFormat = ImpInsertFormat( rCode, nPos,
		bAfterLoadingSO5, nOrgIndex );
	if (pNewFormat)
		pNewFormat->SetNewStandardDefined( nVersion );
		// so that it gets saved, displayed properly, and converted by old versions
	return pNewFormat;
}

void SvNumberFormatter::GetFormatSpecialInfo(sal_uInt32 nFormat,
											 sal_Bool& bThousand,
											 sal_Bool& IsRed,
											 sal_uInt16& nPrecision,
											 sal_uInt16& nAnzLeading)

{
	const SvNumberformat* pFormat = aFTable.Get(nFormat);
	if (pFormat)
		pFormat->GetFormatSpecialInfo(bThousand, IsRed,
									  nPrecision, nAnzLeading);
	else
	{
		bThousand = sal_False;
		IsRed = sal_False;
		nPrecision = pFormatScanner->GetStandardPrec();
		nAnzLeading = 0;
	}
}

sal_uInt16 SvNumberFormatter::GetFormatPrecision( sal_uInt32 nFormat ) const
{
	const SvNumberformat* pFormat = aFTable.Get( nFormat );
	if ( pFormat )
		return pFormat->GetFormatPrecision();
	else
		return pFormatScanner->GetStandardPrec();
}


String SvNumberFormatter::GetFormatDecimalSep( sal_uInt32 nFormat ) const
{
	const SvNumberformat* pFormat = aFTable.Get( nFormat );
	if ( !pFormat || pFormat->GetLanguage() == ActLnge )
        return GetNumDecimalSep();

    String aRet;
    LanguageType eSaveLang = xLocaleData.getCurrentLanguage();
    if ( pFormat->GetLanguage() == eSaveLang )
        aRet = xLocaleData->getNumDecimalSep();
    else
    {
        ::com::sun::star::lang::Locale aSaveLocale( xLocaleData->getLocale() );
        ::com::sun::star::lang::Locale aTmpLocale(MsLangId::convertLanguageToLocale(pFormat->GetLanguage()));
        ((SvNumberFormatter*)this)->xLocaleData.changeLocale(aTmpLocale, pFormat->GetLanguage() );
        aRet = xLocaleData->getNumDecimalSep();
        ((SvNumberFormatter*)this)->xLocaleData.changeLocale( aSaveLocale, eSaveLang );
    }
	return aRet;
}


sal_uInt32 SvNumberFormatter::GetFormatSpecialInfo( const String& rFormatString,
			sal_Bool& bThousand, sal_Bool& IsRed, sal_uInt16& nPrecision,
			sal_uInt16& nAnzLeading, LanguageType eLnge )

{
	xub_StrLen nCheckPos = 0;
	if (eLnge == LANGUAGE_DONTKNOW)
        eLnge = IniLnge;
	ChangeIntl(eLnge);									// ggfs. austauschen
	eLnge = ActLnge;
	String aTmpStr( rFormatString );
	SvNumberformat* pFormat = new SvNumberformat( aTmpStr,
		pFormatScanner, pStringScanner, nCheckPos, eLnge );
	if ( nCheckPos == 0 )
		pFormat->GetFormatSpecialInfo( bThousand, IsRed, nPrecision, nAnzLeading );
	else
	{
		bThousand = sal_False;
		IsRed = sal_False;
		nPrecision = pFormatScanner->GetStandardPrec();
		nAnzLeading = 0;
	}
	delete pFormat;
	return nCheckPos;
}


inline sal_uInt32 SetIndexTable( NfIndexTableOffset nTabOff, sal_uInt32 nIndOff )
{
	if ( !bIndexTableInitialized )
	{
		DBG_ASSERT( theIndexTable[nTabOff] == NUMBERFORMAT_ENTRY_NOT_FOUND,
			"SetIndexTable: theIndexTable[nTabOff] already occupied" );
		theIndexTable[nTabOff] = nIndOff;
	}
	return nIndOff;
}


sal_Int32 SvNumberFormatter::ImpGetFormatCodeIndex(
			::com::sun::star::uno::Sequence< ::com::sun::star::i18n::NumberFormatCode >& rSeq,
			const NfIndexTableOffset nTabOff )
{
	const sal_Int32 nLen = rSeq.getLength();
	for ( sal_Int32 j=0; j<nLen; j++ )
	{
		if ( rSeq[j].Index == nTabOff )
			return j;
	}
    if (LocaleDataWrapper::areChecksEnabled() && (nTabOff < NF_CURRENCY_START
                || NF_CURRENCY_END < nTabOff || nTabOff == NF_CURRENCY_1000INT
                || nTabOff == NF_CURRENCY_1000INT_RED
                || nTabOff == NF_CURRENCY_1000DEC2_CCC))
	{	// currency entries with decimals might not exist, e.g. Italian Lira
		String aMsg( RTL_CONSTASCII_USTRINGPARAM(
                    "SvNumberFormatter::ImpGetFormatCodeIndex: not found: "));
		aMsg += String::CreateFromInt32( nTabOff );
        LocaleDataWrapper::outputCheckMessage( xLocaleData->appendLocaleInfo(
                    aMsg));
	}
	if ( nLen )
	{
		sal_Int32 j;
		// look for a preset default
		for ( j=0; j<nLen; j++ )
		{
			if ( rSeq[j].Default )
				return j;
		}
		// currencies are special, not all format codes must exist, but all
		// builtin number format key index positions must have a format assigned
		if ( NF_CURRENCY_START <= nTabOff && nTabOff <= NF_CURRENCY_END )
		{
			// look for a format with decimals
			for ( j=0; j<nLen; j++ )
			{
				if ( rSeq[j].Index == NF_CURRENCY_1000DEC2 )
					return j;
			}
			// last resort: look for a format without decimals
			for ( j=0; j<nLen; j++ )
			{
				if ( rSeq[j].Index == NF_CURRENCY_1000INT )
					return j;
			}
		}
	}
	else
	{	// we need at least _some_ format
		rSeq.realloc(1);
		rSeq[0] = ::com::sun::star::i18n::NumberFormatCode();
		String aTmp( '0' );
        aTmp += GetNumDecimalSep();
		aTmp.AppendAscii( RTL_CONSTASCII_STRINGPARAM( "############" ) );
		rSeq[0].Code = aTmp;
	}
	return 0;
}


sal_Int32 SvNumberFormatter::ImpAdjustFormatCodeDefault(
        ::com::sun::star::i18n::NumberFormatCode * pFormatArr,
        sal_Int32 nCnt, sal_Bool bCheckCorrectness )
{
	using namespace ::com::sun::star;

    if ( !nCnt )
        return -1;
    if (bCheckCorrectness && LocaleDataWrapper::areChecksEnabled())
    {   // check the locale data for correctness
        ByteString aMsg;
        sal_Int32 nElem, nShort, nMedium, nLong, nShortDef, nMediumDef, nLongDef;
        nShort = nMedium = nLong = nShortDef = nMediumDef = nLongDef = -1;
        for ( nElem = 0; nElem < nCnt; nElem++ )
        {
            switch ( pFormatArr[nElem].Type )
            {
                case i18n::KNumberFormatType::SHORT :
                    nShort = nElem;
                break;
                case i18n::KNumberFormatType::MEDIUM :
                    nMedium = nElem;
                break;
                case i18n::KNumberFormatType::LONG :
                    nLong = nElem;
                break;
                default:
                    aMsg = "unknown type";
            }
            if ( pFormatArr[nElem].Default )
            {
                switch ( pFormatArr[nElem].Type )
                {
                    case i18n::KNumberFormatType::SHORT :
                        if ( nShortDef != -1 )
                            aMsg = "dupe short type default";
                        nShortDef = nElem;
                    break;
                    case i18n::KNumberFormatType::MEDIUM :
                        if ( nMediumDef != -1 )
                            aMsg = "dupe medium type default";
                        nMediumDef = nElem;
                    break;
                    case i18n::KNumberFormatType::LONG :
                        if ( nLongDef != -1 )
                            aMsg = "dupe long type default";
                        nLongDef = nElem;
                    break;
                }
            }
            if ( aMsg.Len() )
            {
                aMsg.Insert( "SvNumberFormatter::ImpAdjustFormatCodeDefault: ", 0 );
                aMsg += "\nXML locale data FormatElement formatindex: ";
                aMsg += ByteString::CreateFromInt32( pFormatArr[nElem].Index );
                String aUMsg( aMsg, RTL_TEXTENCODING_ASCII_US);
                LocaleDataWrapper::outputCheckMessage(
                        xLocaleData->appendLocaleInfo( aUMsg));
                aMsg.Erase();
            }
        }
        if ( nShort != -1 && nShortDef == -1 )
            aMsg += "no short type default  ";
        if ( nMedium != -1 && nMediumDef == -1 )
            aMsg += "no medium type default  ";
        if ( nLong != -1 && nLongDef == -1 )
            aMsg += "no long type default  ";
        if ( aMsg.Len() )
        {
            aMsg.Insert( "SvNumberFormatter::ImpAdjustFormatCodeDefault: ", 0 );
            aMsg += "\nXML locale data FormatElement group of: ";
            String aUMsg( aMsg, RTL_TEXTENCODING_ASCII_US);
            aUMsg += String( pFormatArr[0].NameID );
            LocaleDataWrapper::outputCheckMessage(
                    xLocaleData->appendLocaleInfo( aUMsg));
            aMsg.Erase();
        }
    }
    // find the default (medium preferred, then long) and reset all other defaults
    sal_Int32 nElem, nDef, nMedium;
    nDef = nMedium = -1;
	for ( nElem = 0; nElem < nCnt; nElem++ )
	{
        if ( pFormatArr[nElem].Default )
        {
            switch ( pFormatArr[nElem].Type )
            {
                case i18n::KNumberFormatType::MEDIUM :
                    nDef = nMedium = nElem;
                break;
                case i18n::KNumberFormatType::LONG :
                    if ( nMedium == -1 )
                        nDef = nElem;
                // fallthru
                default:
                    if ( nDef == -1 )
                        nDef = nElem;
                    pFormatArr[nElem].Default = sal_False;
            }
        }
	}
    if ( nDef == -1 )
        nDef = 0;
    pFormatArr[nDef].Default = sal_True;
    return nDef;
}


void SvNumberFormatter::ImpGenerateFormats( sal_uInt32 CLOffset, sal_Bool bLoadingSO5 )
{
	using namespace ::com::sun::star;

	if ( !bIndexTableInitialized )
	{
		for ( sal_uInt16 j=0; j<NF_INDEX_TABLE_ENTRIES; j++ )
		{
			theIndexTable[j] = NUMBERFORMAT_ENTRY_NOT_FOUND;
		}
	}
	sal_Bool bOldConvertMode = pFormatScanner->GetConvertMode();
	if (bOldConvertMode)
		pFormatScanner->SetConvertMode(sal_False);		// switch off for this function

	NumberFormatCodeWrapper aNumberFormatCode( xServiceManager, GetLocale() );

	xub_StrLen nCheckPos = 0;
	SvNumberformat* pNewFormat = NULL;
	String aFormatCode;
	sal_Int32 nIdx;
	sal_Bool bDefault;

	// Counter for additional builtin formats not fitting into the first 10
	// of a category (TLOT:=The Legacy Of Templin), altogether about 20 formats.
	// Has to be incremented on each ImpInsertNewStandardformat, new formats
	// must be appended, not inserted!
	sal_uInt16 nNewExtended = ZF_STANDARD_NEWEXTENDED;

    // Number
    uno::Sequence< i18n::NumberFormatCode > aFormatSeq
        = aNumberFormatCode.getAllFormatCode( i18n::KNumberFormatUsage::FIXED_NUMBER );
    ImpAdjustFormatCodeDefault( aFormatSeq.getArray(), aFormatSeq.getLength() );

    // General
    nIdx = ImpGetFormatCodeIndex( aFormatSeq, NF_NUMBER_STANDARD );
    SvNumberformat* pStdFormat = ImpInsertFormat( aFormatSeq[nIdx],
            CLOffset + SetIndexTable( NF_NUMBER_STANDARD, ZF_STANDARD ));
    if (pStdFormat)
    {
        // This is _the_ standard format.
        if (LocaleDataWrapper::areChecksEnabled() &&
                pStdFormat->GetType() != NUMBERFORMAT_NUMBER)
        {
            String aMsg( RTL_CONSTASCII_USTRINGPARAM(
                        "SvNumberFormatter::ImpGenerateFormats: General format not NUMBER"));
            LocaleDataWrapper::outputCheckMessage(
                    xLocaleData->appendLocaleInfo( aMsg));
        }
        pStdFormat->SetType( NUMBERFORMAT_NUMBER );
        pStdFormat->SetStandard();
        pStdFormat->SetLastInsertKey( SV_MAX_ANZ_STANDARD_FORMATE );
    }
    else
    {
        if (LocaleDataWrapper::areChecksEnabled())
        {
            String aMsg( RTL_CONSTASCII_USTRINGPARAM(
                        "SvNumberFormatter::ImpGenerateFormats: General format not insertable, nothing will work"));
            LocaleDataWrapper::outputCheckMessage(
                    xLocaleData->appendLocaleInfo( aMsg));
        }
    }

	// Boolean
	aFormatCode = pFormatScanner->GetBooleanString();
	pNewFormat = new SvNumberformat( aFormatCode,
		pFormatScanner, pStringScanner,	nCheckPos, ActLnge );
	pNewFormat->SetType(NUMBERFORMAT_LOGICAL);
	pNewFormat->SetStandard();
	if ( !aFTable.Insert(
			CLOffset + SetIndexTable( NF_BOOLEAN, ZF_STANDARD_LOGICAL ),
			pNewFormat))
		delete pNewFormat;

	// Text
	aFormatCode = '@';
	pNewFormat = new SvNumberformat( aFormatCode,
		pFormatScanner, pStringScanner, nCheckPos, ActLnge );
	pNewFormat->SetType(NUMBERFORMAT_TEXT);
	pNewFormat->SetStandard();
	if ( !aFTable.Insert(
			CLOffset + SetIndexTable( NF_TEXT, ZF_STANDARD_TEXT ),
			pNewFormat))
		delete pNewFormat;



	// 0
	nIdx = ImpGetFormatCodeIndex( aFormatSeq, NF_NUMBER_INT );
	ImpInsertFormat( aFormatSeq[nIdx],
		CLOffset + SetIndexTable( NF_NUMBER_INT, ZF_STANDARD+1 ));

	// 0.00
	nIdx = ImpGetFormatCodeIndex( aFormatSeq, NF_NUMBER_DEC2 );
	ImpInsertFormat( aFormatSeq[nIdx],
		CLOffset + SetIndexTable( NF_NUMBER_DEC2, ZF_STANDARD+2 ));

	// #,##0
	nIdx = ImpGetFormatCodeIndex( aFormatSeq, NF_NUMBER_1000INT );
	ImpInsertFormat( aFormatSeq[nIdx],
			CLOffset + SetIndexTable( NF_NUMBER_1000INT, ZF_STANDARD+3 ));

	// #,##0.00
	nIdx = ImpGetFormatCodeIndex( aFormatSeq, NF_NUMBER_1000DEC2 );
	ImpInsertFormat( aFormatSeq[nIdx],
		CLOffset + SetIndexTable( NF_NUMBER_1000DEC2, ZF_STANDARD+4 ));

	// #.##0,00 System country/language dependent   since number formatter version 6
	nIdx = ImpGetFormatCodeIndex( aFormatSeq, NF_NUMBER_SYSTEM );
	ImpInsertNewStandardFormat( aFormatSeq[nIdx],
		CLOffset + SetIndexTable( NF_NUMBER_SYSTEM, ZF_STANDARD+5 ),
		SV_NUMBERFORMATTER_VERSION_NEWSTANDARD );


	// Percent number
	aFormatSeq = aNumberFormatCode.getAllFormatCode( i18n::KNumberFormatUsage::PERCENT_NUMBER );
    ImpAdjustFormatCodeDefault( aFormatSeq.getArray(), aFormatSeq.getLength() );

	// 0%
	nIdx = ImpGetFormatCodeIndex( aFormatSeq, NF_PERCENT_INT );
	ImpInsertFormat( aFormatSeq[nIdx],
		CLOffset + SetIndexTable( NF_PERCENT_INT, ZF_STANDARD_PERCENT ));

	// 0.00%
	nIdx = ImpGetFormatCodeIndex( aFormatSeq, NF_PERCENT_DEC2 );
	ImpInsertFormat( aFormatSeq[nIdx],
		CLOffset + SetIndexTable( NF_PERCENT_DEC2, ZF_STANDARD_PERCENT+1 ));



    // Currency. NO default standard option! Default is determined of locale
    // data default currency and format is generated if needed.
	aFormatSeq = aNumberFormatCode.getAllFormatCode( i18n::KNumberFormatUsage::CURRENCY );
    if (LocaleDataWrapper::areChecksEnabled())
    {
        // though no default desired here, test for correctness of locale data
        ImpAdjustFormatCodeDefault( aFormatSeq.getArray(), aFormatSeq.getLength() );
    }

	// #,##0
	nIdx = ImpGetFormatCodeIndex( aFormatSeq, NF_CURRENCY_1000INT );
	bDefault = aFormatSeq[nIdx].Default;
	aFormatSeq[nIdx].Default = sal_False;
	ImpInsertFormat( aFormatSeq[nIdx],
		CLOffset + SetIndexTable( NF_CURRENCY_1000INT, ZF_STANDARD_CURRENCY ));
	aFormatSeq[nIdx].Default = bDefault;

	// #,##0.00
	nIdx = ImpGetFormatCodeIndex( aFormatSeq, NF_CURRENCY_1000DEC2 );
	bDefault = aFormatSeq[nIdx].Default;
	aFormatSeq[nIdx].Default = sal_False;
	ImpInsertFormat( aFormatSeq[nIdx],
		CLOffset + SetIndexTable( NF_CURRENCY_1000DEC2, ZF_STANDARD_CURRENCY+1 ));
	aFormatSeq[nIdx].Default = bDefault;

	// #,##0 negative red
	nIdx = ImpGetFormatCodeIndex( aFormatSeq, NF_CURRENCY_1000INT_RED );
	bDefault = aFormatSeq[nIdx].Default;
	aFormatSeq[nIdx].Default = sal_False;
	ImpInsertFormat( aFormatSeq[nIdx],
		CLOffset + SetIndexTable( NF_CURRENCY_1000INT_RED, ZF_STANDARD_CURRENCY+2 ));
	aFormatSeq[nIdx].Default = bDefault;

	// #,##0.00 negative red
	nIdx = ImpGetFormatCodeIndex( aFormatSeq, NF_CURRENCY_1000DEC2_RED );
	bDefault = aFormatSeq[nIdx].Default;
	aFormatSeq[nIdx].Default = sal_False;
	ImpInsertFormat( aFormatSeq[nIdx],
		CLOffset + SetIndexTable( NF_CURRENCY_1000DEC2_RED, ZF_STANDARD_CURRENCY+3 ));
	aFormatSeq[nIdx].Default = bDefault;

	// #,##0.00 USD   since number formatter version 3
	nIdx = ImpGetFormatCodeIndex( aFormatSeq, NF_CURRENCY_1000DEC2_CCC );
	bDefault = aFormatSeq[nIdx].Default;
	aFormatSeq[nIdx].Default = sal_False;
	pNewFormat = ImpInsertFormat( aFormatSeq[nIdx],
		CLOffset + SetIndexTable( NF_CURRENCY_1000DEC2_CCC, ZF_STANDARD_CURRENCY+4 ));
	if ( pNewFormat )
		pNewFormat->SetUsed(sal_True);		// must be saved for older versions
	aFormatSeq[nIdx].Default = bDefault;

	// #.##0,--   since number formatter version 6
	nIdx = ImpGetFormatCodeIndex( aFormatSeq, NF_CURRENCY_1000DEC2_DASHED );
	bDefault = aFormatSeq[nIdx].Default;
	aFormatSeq[nIdx].Default = sal_False;
	ImpInsertNewStandardFormat( aFormatSeq[nIdx],
		CLOffset + SetIndexTable( NF_CURRENCY_1000DEC2_DASHED, ZF_STANDARD_CURRENCY+5 ),
		SV_NUMBERFORMATTER_VERSION_NEWSTANDARD );
	aFormatSeq[nIdx].Default = bDefault;



	// Date
	aFormatSeq = aNumberFormatCode.getAllFormatCode( i18n::KNumberFormatUsage::DATE );
    ImpAdjustFormatCodeDefault( aFormatSeq.getArray(), aFormatSeq.getLength() );

	// DD.MM.YY   System
	nIdx = ImpGetFormatCodeIndex( aFormatSeq, NF_DATE_SYSTEM_SHORT );
	ImpInsertFormat( aFormatSeq[nIdx],
		CLOffset + SetIndexTable( NF_DATE_SYSTEM_SHORT, ZF_STANDARD_DATE ));

	// NN DD.MMM YY
	nIdx = ImpGetFormatCodeIndex( aFormatSeq, NF_DATE_DEF_NNDDMMMYY );
	ImpInsertFormat( aFormatSeq[nIdx],
		CLOffset + SetIndexTable( NF_DATE_DEF_NNDDMMMYY, ZF_STANDARD_DATE+1 ));

	// DD.MM.YY   def/System
	nIdx = ImpGetFormatCodeIndex( aFormatSeq, NF_DATE_SYS_MMYY );
	ImpInsertFormat( aFormatSeq[nIdx],
		CLOffset + SetIndexTable( NF_DATE_SYS_MMYY, ZF_STANDARD_DATE+2 ));

	// DD MMM
	nIdx = ImpGetFormatCodeIndex( aFormatSeq, NF_DATE_SYS_DDMMM );
	ImpInsertFormat( aFormatSeq[nIdx],
		CLOffset + SetIndexTable( NF_DATE_SYS_DDMMM, ZF_STANDARD_DATE+3 ));

	// MMMM
	nIdx = ImpGetFormatCodeIndex( aFormatSeq, NF_DATE_MMMM );
	ImpInsertFormat( aFormatSeq[nIdx],
		CLOffset + SetIndexTable( NF_DATE_MMMM, ZF_STANDARD_DATE+4 ));

	// QQ YY
	nIdx = ImpGetFormatCodeIndex( aFormatSeq, NF_DATE_QQJJ );
	ImpInsertFormat( aFormatSeq[nIdx],
		CLOffset + SetIndexTable( NF_DATE_QQJJ, ZF_STANDARD_DATE+5 ));

	// DD.MM.YYYY   since number formatter version 2, was DD.MM.[YY]YY
	nIdx = ImpGetFormatCodeIndex( aFormatSeq, NF_DATE_SYS_DDMMYYYY );
	pNewFormat = ImpInsertFormat( aFormatSeq[nIdx],
		CLOffset + SetIndexTable( NF_DATE_SYS_DDMMYYYY, ZF_STANDARD_DATE+6 ));
	if ( pNewFormat )
		pNewFormat->SetUsed(sal_True);		// must be saved for older versions

	// DD.MM.YY   def/System, since number formatter version 6
	nIdx = ImpGetFormatCodeIndex( aFormatSeq, NF_DATE_SYS_DDMMYY );
	ImpInsertNewStandardFormat( aFormatSeq[nIdx],
		CLOffset + SetIndexTable( NF_DATE_SYS_DDMMYY, ZF_STANDARD_DATE+7 ),
		SV_NUMBERFORMATTER_VERSION_NEWSTANDARD );

	// NNN, D. MMMM YYYY   System
	// Long day of week: "NNNN" instead of "NNN," because of compatibility
	nIdx = ImpGetFormatCodeIndex( aFormatSeq, NF_DATE_SYSTEM_LONG );
	ImpInsertNewStandardFormat( aFormatSeq[nIdx],
		CLOffset + SetIndexTable( NF_DATE_SYSTEM_LONG, ZF_STANDARD_DATE+8 ),
		SV_NUMBERFORMATTER_VERSION_NEWSTANDARD );

	// Hard coded but system (regional settings) delimiters dependent long date formats
	// since numberformatter version 6

	// D. MMM YY   def/System
	nIdx = ImpGetFormatCodeIndex( aFormatSeq, NF_DATE_SYS_DMMMYY );
	ImpInsertNewStandardFormat( aFormatSeq[nIdx],
		CLOffset + SetIndexTable( NF_DATE_SYS_DMMMYY, ZF_STANDARD_DATE+9 ),
		SV_NUMBERFORMATTER_VERSION_NEWSTANDARD );

	//! Unfortunately TLOT intended only 10 builtin formats per category, more
	//! would overwrite the next category (ZF_STANDARD_TIME) :-((
	//! Therefore they are inserted with nNewExtended++ (which is also limited)

	// D. MMM YYYY   def/System
	nIdx = ImpGetFormatCodeIndex( aFormatSeq, NF_DATE_SYS_DMMMYYYY );
	ImpInsertNewStandardFormat( aFormatSeq[nIdx],
		CLOffset + SetIndexTable( NF_DATE_SYS_DMMMYYYY, nNewExtended++ ),
		SV_NUMBERFORMATTER_VERSION_NEWSTANDARD );

	// D. MMMM YYYY   def/System
	nIdx = ImpGetFormatCodeIndex( aFormatSeq, NF_DATE_SYS_DMMMMYYYY );
	ImpInsertNewStandardFormat( aFormatSeq[nIdx],
		CLOffset + SetIndexTable( NF_DATE_SYS_DMMMMYYYY, nNewExtended++ ),
		SV_NUMBERFORMATTER_VERSION_NEWSTANDARD );

	// NN, D. MMM YY   def/System
	nIdx = ImpGetFormatCodeIndex( aFormatSeq, NF_DATE_SYS_NNDMMMYY );
	ImpInsertNewStandardFormat( aFormatSeq[nIdx],
		CLOffset + SetIndexTable( NF_DATE_SYS_NNDMMMYY, nNewExtended++ ),
		SV_NUMBERFORMATTER_VERSION_NEWSTANDARD );

	// NN, D. MMMM YYYY   def/System
	nIdx = ImpGetFormatCodeIndex( aFormatSeq, NF_DATE_SYS_NNDMMMMYYYY );
	ImpInsertNewStandardFormat( aFormatSeq[nIdx],
		CLOffset + SetIndexTable( NF_DATE_SYS_NNDMMMMYYYY, nNewExtended++ ),
		SV_NUMBERFORMATTER_VERSION_NEWSTANDARD );

	// NNN, D. MMMM YYYY   def/System
	nIdx = ImpGetFormatCodeIndex( aFormatSeq, NF_DATE_SYS_NNNNDMMMMYYYY );
	ImpInsertNewStandardFormat( aFormatSeq[nIdx],
		CLOffset + SetIndexTable( NF_DATE_SYS_NNNNDMMMMYYYY, nNewExtended++ ),
		SV_NUMBERFORMATTER_VERSION_NEWSTANDARD );

	// Hard coded DIN (Deutsche Industrie Norm) and EN (European Norm) date formats

	// D. MMM. YYYY   DIN/EN
	nIdx = ImpGetFormatCodeIndex( aFormatSeq, NF_DATE_DIN_DMMMYYYY );
	ImpInsertNewStandardFormat( aFormatSeq[nIdx],
		CLOffset + SetIndexTable( NF_DATE_DIN_DMMMYYYY, nNewExtended++ ),
		SV_NUMBERFORMATTER_VERSION_NEWSTANDARD );

	// D. MMMM YYYY   DIN/EN
	nIdx = ImpGetFormatCodeIndex( aFormatSeq, NF_DATE_DIN_DMMMMYYYY );
	ImpInsertNewStandardFormat( aFormatSeq[nIdx],
		CLOffset + SetIndexTable( NF_DATE_DIN_DMMMMYYYY, nNewExtended++ ),
		SV_NUMBERFORMATTER_VERSION_NEWSTANDARD );

	// MM-DD   DIN/EN
	nIdx = ImpGetFormatCodeIndex( aFormatSeq, NF_DATE_DIN_MMDD );
	ImpInsertNewStandardFormat( aFormatSeq[nIdx],
		CLOffset + SetIndexTable( NF_DATE_DIN_MMDD, nNewExtended++ ),
		SV_NUMBERFORMATTER_VERSION_NEWSTANDARD );

	// YY-MM-DD   DIN/EN
	nIdx = ImpGetFormatCodeIndex( aFormatSeq, NF_DATE_DIN_YYMMDD );
	ImpInsertNewStandardFormat( aFormatSeq[nIdx],
		CLOffset + SetIndexTable( NF_DATE_DIN_YYMMDD, nNewExtended++ ),
		SV_NUMBERFORMATTER_VERSION_NEWSTANDARD );

	// YYYY-MM-DD   DIN/EN
	nIdx = ImpGetFormatCodeIndex( aFormatSeq, NF_DATE_DIN_YYYYMMDD );
	ImpInsertNewStandardFormat( aFormatSeq[nIdx],
		CLOffset + SetIndexTable( NF_DATE_DIN_YYYYMMDD, nNewExtended++ ),
		SV_NUMBERFORMATTER_VERSION_NEWSTANDARD );



	// Time
	aFormatSeq = aNumberFormatCode.getAllFormatCode( i18n::KNumberFormatUsage::TIME );
    ImpAdjustFormatCodeDefault( aFormatSeq.getArray(), aFormatSeq.getLength() );

	// HH:MM
	nIdx = ImpGetFormatCodeIndex( aFormatSeq, NF_TIME_HHMM );
	ImpInsertFormat( aFormatSeq[nIdx],
		CLOffset + SetIndexTable( NF_TIME_HHMM, ZF_STANDARD_TIME ));

	// HH:MM:SS
	nIdx = ImpGetFormatCodeIndex( aFormatSeq, NF_TIME_HHMMSS );
	ImpInsertFormat( aFormatSeq[nIdx],
		CLOffset + SetIndexTable( NF_TIME_HHMMSS, ZF_STANDARD_TIME+1 ));

	// HH:MM AM/PM
	nIdx = ImpGetFormatCodeIndex( aFormatSeq, NF_TIME_HHMMAMPM );
	ImpInsertFormat( aFormatSeq[nIdx],
		CLOffset + SetIndexTable( NF_TIME_HHMMAMPM, ZF_STANDARD_TIME+2 ));

	// HH:MM:SS AM/PM
	nIdx = ImpGetFormatCodeIndex( aFormatSeq, NF_TIME_HHMMSSAMPM );
	ImpInsertFormat( aFormatSeq[nIdx],
		CLOffset + SetIndexTable( NF_TIME_HHMMSSAMPM, ZF_STANDARD_TIME+3 ));

	// [HH]:MM:SS
	nIdx = ImpGetFormatCodeIndex( aFormatSeq, NF_TIME_HH_MMSS );
	ImpInsertFormat( aFormatSeq[nIdx],
		CLOffset + SetIndexTable( NF_TIME_HH_MMSS, ZF_STANDARD_TIME+4 ));

	// MM:SS,00
	nIdx = ImpGetFormatCodeIndex( aFormatSeq, NF_TIME_MMSS00 );
	ImpInsertFormat( aFormatSeq[nIdx],
		CLOffset + SetIndexTable( NF_TIME_MMSS00, ZF_STANDARD_TIME+5 ));

	// [HH]:MM:SS,00
	nIdx = ImpGetFormatCodeIndex( aFormatSeq, NF_TIME_HH_MMSS00 );
	ImpInsertNewStandardFormat( aFormatSeq[nIdx],
		CLOffset + SetIndexTable( NF_TIME_HH_MMSS00, ZF_STANDARD_TIME+6 ),
		SV_NUMBERFORMATTER_VERSION_NF_TIME_HH_MMSS00 );



	// DateTime
	aFormatSeq = aNumberFormatCode.getAllFormatCode( i18n::KNumberFormatUsage::DATE_TIME );
    ImpAdjustFormatCodeDefault( aFormatSeq.getArray(), aFormatSeq.getLength() );

	// DD.MM.YY HH:MM   System
	nIdx = ImpGetFormatCodeIndex( aFormatSeq, NF_DATETIME_SYSTEM_SHORT_HHMM );
	ImpInsertFormat( aFormatSeq[nIdx],
		CLOffset + SetIndexTable( NF_DATETIME_SYSTEM_SHORT_HHMM, ZF_STANDARD_DATETIME ));

	// DD.MM.YYYY HH:MM:SS   System
	nIdx = ImpGetFormatCodeIndex( aFormatSeq, NF_DATETIME_SYS_DDMMYYYY_HHMMSS );
	ImpInsertNewStandardFormat( aFormatSeq[nIdx],
		CLOffset + SetIndexTable( NF_DATETIME_SYS_DDMMYYYY_HHMMSS, ZF_STANDARD_DATETIME+1 ),
		SV_NUMBERFORMATTER_VERSION_NF_DATETIME_SYS_DDMMYYYY_HHMMSS );



	// Scientific number
	aFormatSeq = aNumberFormatCode.getAllFormatCode( i18n::KNumberFormatUsage::SCIENTIFIC_NUMBER );
    ImpAdjustFormatCodeDefault( aFormatSeq.getArray(), aFormatSeq.getLength() );

	// 0.00E+000
	nIdx = ImpGetFormatCodeIndex( aFormatSeq, NF_SCIENTIFIC_000E000 );
	ImpInsertFormat( aFormatSeq[nIdx],
		CLOffset + SetIndexTable( NF_SCIENTIFIC_000E000, ZF_STANDARD_SCIENTIFIC ));

	// 0.00E+00
	nIdx = ImpGetFormatCodeIndex( aFormatSeq, NF_SCIENTIFIC_000E00 );
	ImpInsertFormat( aFormatSeq[nIdx],
		CLOffset + SetIndexTable( NF_SCIENTIFIC_000E00, ZF_STANDARD_SCIENTIFIC+1 ));



	// Fraction number (no default option)
	i18n::NumberFormatCode aSingleFormatCode;

	 // # ?/?
	aSingleFormatCode.Code = ::rtl::OUString( RTL_CONSTASCII_USTRINGPARAM( "# ?/?" ) );
	String s25( RTL_CONSTASCII_USTRINGPARAM( "# ?/?" ) );			// # ?/?
	ImpInsertFormat( aSingleFormatCode,
		CLOffset + SetIndexTable( NF_FRACTION_1, ZF_STANDARD_FRACTION ));

	// # ??/??
	//! "??/" would be interpreted by the compiler as a trigraph for '\'
	aSingleFormatCode.Code = ::rtl::OUString( RTL_CONSTASCII_USTRINGPARAM( "# ?\?/?\?" ) );
	ImpInsertFormat( aSingleFormatCode,
		CLOffset + SetIndexTable( NF_FRACTION_2, ZF_STANDARD_FRACTION+1 ));

	// Week of year   must be appended here because of nNewExtended
    const NfKeywordTable & rKeyword = pFormatScanner->GetKeywords();
	aSingleFormatCode.Code = rKeyword[NF_KEY_WW];
	ImpInsertNewStandardFormat( aSingleFormatCode,
		CLOffset + SetIndexTable( NF_DATE_WW, nNewExtended++ ),
		SV_NUMBERFORMATTER_VERSION_NF_DATE_WW );



	bIndexTableInitialized = sal_True;
	DBG_ASSERT( nNewExtended <= ZF_STANDARD_NEWEXTENDEDMAX,
		"ImpGenerateFormats: overflow of nNewExtended standard formats" );

	// Now all additional format codes provided by I18N, but only if not
	// loading from old SO5 file format, then they are appended last.
	if ( !bLoadingSO5 )
		ImpGenerateAdditionalFormats( CLOffset, aNumberFormatCode, sal_False );

	if (bOldConvertMode)
		pFormatScanner->SetConvertMode(sal_True);
}


void SvNumberFormatter::ImpGenerateAdditionalFormats( sal_uInt32 CLOffset,
			NumberFormatCodeWrapper& rNumberFormatCode, sal_Bool bAfterLoadingSO5 )
{
	using namespace ::com::sun::star;

	SvNumberformat* pStdFormat =
		(SvNumberformat*) aFTable.Get( CLOffset + ZF_STANDARD );
	if ( !pStdFormat )
	{
		DBG_ERRORFILE( "ImpGenerateAdditionalFormats: no GENERAL format" );
		return ;
	}
	sal_uInt32 nPos = CLOffset + pStdFormat->GetLastInsertKey();
	rNumberFormatCode.setLocale( GetLocale() );
	sal_Int32 j;

	// All currencies, this time with [$...] which was stripped in
	// ImpGenerateFormats for old "automatic" currency formats.
	uno::Sequence< i18n::NumberFormatCode > aFormatSeq =
		rNumberFormatCode.getAllFormatCode( i18n::KNumberFormatUsage::CURRENCY );
    i18n::NumberFormatCode * pFormatArr = aFormatSeq.getArray();
	sal_Int32 nCodes = aFormatSeq.getLength();
    ImpAdjustFormatCodeDefault( aFormatSeq.getArray(), nCodes );
	for ( j = 0; j < nCodes; j++ )
	{
		if ( nPos - CLOffset >= SV_COUNTRY_LANGUAGE_OFFSET )
		{
			DBG_ERRORFILE( "ImpGenerateAdditionalFormats: too many formats" );
			break;	// for
		}
        if ( pFormatArr[j].Index < NF_INDEX_TABLE_ENTRIES &&
                pFormatArr[j].Index != NF_CURRENCY_1000DEC2_CCC )
		{	// Insert only if not already inserted, but internal index must be
			// above so ImpInsertFormat can distinguish it.
            sal_Int16 nOrgIndex = pFormatArr[j].Index;
            pFormatArr[j].Index = sal::static_int_cast< sal_Int16 >(
                pFormatArr[j].Index + nCodes + NF_INDEX_TABLE_ENTRIES);
            //! no default on currency
            sal_Bool bDefault = aFormatSeq[j].Default;
            aFormatSeq[j].Default = sal_False;
            if ( ImpInsertNewStandardFormat( pFormatArr[j], nPos+1,
					SV_NUMBERFORMATTER_VERSION_ADDITIONAL_I18N_FORMATS,
					bAfterLoadingSO5, nOrgIndex ) )
				nPos++;
            pFormatArr[j].Index = nOrgIndex;
            aFormatSeq[j].Default = bDefault;
		}
	}

	// all additional format codes provided by I18N that are not old standard index
	aFormatSeq = rNumberFormatCode.getAllFormatCodes();
	nCodes = aFormatSeq.getLength();
    if ( nCodes )
    {
        pFormatArr = aFormatSeq.getArray();
        // don't check ALL
        sal_Int32 nDef = ImpAdjustFormatCodeDefault( pFormatArr, nCodes, sal_False);
        // don't have any defaults here
        pFormatArr[nDef].Default = sal_False;
        for ( j = 0; j < nCodes; j++ )
        {
            if ( nPos - CLOffset >= SV_COUNTRY_LANGUAGE_OFFSET )
            {
                DBG_ERRORFILE( "ImpGenerateAdditionalFormats: too many formats" );
                break;  // for
            }
            if ( pFormatArr[j].Index >= NF_INDEX_TABLE_ENTRIES )
                if ( ImpInsertNewStandardFormat( pFormatArr[j], nPos+1,
                        SV_NUMBERFORMATTER_VERSION_ADDITIONAL_I18N_FORMATS,
                        bAfterLoadingSO5 ) )
                    nPos++;
        }
    }

	pStdFormat->SetLastInsertKey( (sal_uInt16)(nPos - CLOffset) );
}


void SvNumberFormatter::ImpGetPosCurrFormat( String& sPosStr, const String& rCurrSymbol )
{
	NfCurrencyEntry::CompletePositiveFormatString( sPosStr,
        rCurrSymbol, xLocaleData->getCurrPositiveFormat() );
}

void SvNumberFormatter::ImpGetNegCurrFormat( String& sNegStr, const String& rCurrSymbol )
{
	NfCurrencyEntry::CompleteNegativeFormatString( sNegStr,
        rCurrSymbol, xLocaleData->getCurrNegativeFormat() );
}

void SvNumberFormatter::GenerateFormat(String& sString,
									   sal_uInt32 nIndex,
									   LanguageType eLnge,
									   sal_Bool bThousand,
									   sal_Bool IsRed,
									   sal_uInt16 nPrecision,
									   sal_uInt16 nAnzLeading)
{
	if (eLnge == LANGUAGE_DONTKNOW)
        eLnge = IniLnge;
	short eType = GetType(nIndex);
	sal_uInt16 i;
	ImpGenerateCL(eLnge);				// ggfs. neu Standard-
									// formate anlegen
	sString.Erase();

    utl::DigitGroupingIterator aGrouping( xLocaleData->getDigitGrouping());
    const xub_StrLen nDigitsInFirstGroup = static_cast<xub_StrLen>(aGrouping.get());
    const String& rThSep = GetNumThousandSep();
	if (nAnzLeading == 0)
	{
		if (!bThousand)
			sString += '#';
		else
		{
			sString += '#';
			sString += rThSep;
            sString.Expand( sString.Len() + nDigitsInFirstGroup, '#' );
		}
	}
	else
	{
		for (i = 0; i < nAnzLeading; i++)
		{
			if (bThousand && i > 0 && i == aGrouping.getPos())
            {
				sString.Insert( rThSep, 0 );
                aGrouping.advance();
            }
			sString.Insert('0',0);
		}
		if (bThousand && nAnzLeading < nDigitsInFirstGroup + 1)
		{
			for (i = nAnzLeading; i < nDigitsInFirstGroup + 1; i++)
			{
				if (bThousand && i % nDigitsInFirstGroup == 0)
					sString.Insert( rThSep, 0 );
				sString.Insert('#',0);
			}
		}
	}
	if (nPrecision > 0)
	{
        sString += GetNumDecimalSep();
        sString.Expand( sString.Len() + nPrecision, '0' );
	}
	if (eType == NUMBERFORMAT_PERCENT)
		sString += '%';
	else if (eType == NUMBERFORMAT_CURRENCY)
	{
		String sNegStr = sString;
		String aCurr;
		const NfCurrencyEntry* pEntry;
		sal_Bool bBank;
		if ( GetNewCurrencySymbolString( nIndex, aCurr, &pEntry, &bBank ) )
		{
			if ( pEntry )
			{
				sal_uInt16 nPosiForm = NfCurrencyEntry::GetEffectivePositiveFormat(
                    xLocaleData->getCurrPositiveFormat(),
					pEntry->GetPositiveFormat(), bBank );
				sal_uInt16 nNegaForm = NfCurrencyEntry::GetEffectiveNegativeFormat(
                    xLocaleData->getCurrNegativeFormat(),
					pEntry->GetNegativeFormat(), bBank );
				pEntry->CompletePositiveFormatString( sString, bBank,
					nPosiForm );
				pEntry->CompleteNegativeFormatString( sNegStr, bBank,
					nNegaForm );
			}
			else
            {   // assume currency abbreviation (AKA banking symbol), not symbol
				sal_uInt16 nPosiForm = NfCurrencyEntry::GetEffectivePositiveFormat(
                    xLocaleData->getCurrPositiveFormat(),
                    xLocaleData->getCurrPositiveFormat(), sal_True );
				sal_uInt16 nNegaForm = NfCurrencyEntry::GetEffectiveNegativeFormat(
                    xLocaleData->getCurrNegativeFormat(),
                    xLocaleData->getCurrNegativeFormat(), sal_True );
				NfCurrencyEntry::CompletePositiveFormatString( sString, aCurr,
					nPosiForm );
				NfCurrencyEntry::CompleteNegativeFormatString( sNegStr, aCurr,
					nNegaForm );
			}
		}
		else
        {   // "automatic" old style
            String aSymbol, aAbbrev;
            GetCompatibilityCurrency( aSymbol, aAbbrev );
            ImpGetPosCurrFormat( sString, aSymbol );
            ImpGetNegCurrFormat( sNegStr, aSymbol );
		}
		if (IsRed)
		{
			sString += ';';
			sString += '[';
			sString += pFormatScanner->GetRedString();
			sString += ']';
		}
		else
			sString += ';';
		sString += sNegStr;
	}
	if (IsRed && eType != NUMBERFORMAT_CURRENCY)
	{
		String sTmpStr = sString;
		sTmpStr += ';';
		sTmpStr += '[';
		sTmpStr += pFormatScanner->GetRedString();
		sTmpStr += ']';
		sTmpStr += '-';
		sTmpStr +=sString;
		sString = sTmpStr;
	}
}

sal_Bool SvNumberFormatter::IsUserDefined(const String& sStr,
									  LanguageType eLnge)
{
	if (eLnge == LANGUAGE_DONTKNOW)
        eLnge = IniLnge;
	sal_uInt32 CLOffset = ImpGenerateCL(eLnge);				// ggfs. neu Standard-
													// formate anlegen
	eLnge = ActLnge;
	sal_uInt32 nKey = ImpIsEntry(sStr, CLOffset, eLnge);
	if (nKey == NUMBERFORMAT_ENTRY_NOT_FOUND)
		return sal_True;
	SvNumberformat* pEntry = aFTable.Get(nKey);
	if ( pEntry && ((pEntry->GetType() & NUMBERFORMAT_DEFINED) != 0) )
		return sal_True;
	return sal_False;
}

sal_uInt32 SvNumberFormatter::GetEntryKey(const String& sStr,
									 LanguageType eLnge)
{
	if (eLnge == LANGUAGE_DONTKNOW)
        eLnge = IniLnge;
	sal_uInt32 CLOffset = ImpGenerateCL(eLnge);				// ggfs. neu Standard-
													// formate anlegen
	return ImpIsEntry(sStr, CLOffset, eLnge);
}

sal_uInt32 SvNumberFormatter::GetStandardIndex(LanguageType eLnge)
{
	if (eLnge == LANGUAGE_DONTKNOW)
        eLnge = IniLnge;
	return GetStandardFormat(NUMBERFORMAT_NUMBER, eLnge);
}

short SvNumberFormatter::GetType(sal_uInt32 nFIndex)
{
	short eType;
	SvNumberformat* pFormat = (SvNumberformat*) aFTable.Get(nFIndex);
	if (!pFormat)
		eType = NUMBERFORMAT_UNDEFINED;
	else
	{
		eType = pFormat->GetType() &~NUMBERFORMAT_DEFINED;
		if (eType == 0)
			eType = NUMBERFORMAT_DEFINED;
	}
	return eType;
}

void SvNumberFormatter::ClearMergeTable()
{
    if ( pMergeTable )
    {
        sal_uInt32* pIndex = (sal_uInt32*) pMergeTable->First();
        while (pIndex)
        {
            delete pIndex;
            pIndex = pMergeTable->Next();
        }
        pMergeTable->Clear();
    }
}

SvNumberFormatterIndexTable* SvNumberFormatter::MergeFormatter(SvNumberFormatter& rTable)
{
    if ( pMergeTable )
        ClearMergeTable();
    else
        pMergeTable = new SvNumberFormatterIndexTable;
	sal_uInt32 nCLOffset = 0;
	sal_uInt32 nOldKey, nOffset, nNewKey;
	sal_uInt32* pNewIndex;
	SvNumberformat* pNewEntry;
	SvNumberformat* pFormat = rTable.aFTable.First();
	while (pFormat)
	{
		nOldKey = rTable.aFTable.GetCurKey();
		nOffset = nOldKey % SV_COUNTRY_LANGUAGE_OFFSET;		// relativIndex
		if (nOffset == 0)									// 1. Format von CL
			nCLOffset = ImpGenerateCL(pFormat->GetLanguage());

		if (nOffset <= SV_MAX_ANZ_STANDARD_FORMATE)		// Std.form.
		{
			nNewKey = nCLOffset + nOffset;
			if (!aFTable.Get(nNewKey))					// noch nicht da
			{
//				pNewEntry = new SvNumberformat(*pFormat);	// Copy reicht nicht !!!
				pNewEntry = new SvNumberformat( *pFormat, *pFormatScanner );
				if (!aFTable.Insert(nNewKey, pNewEntry))
					delete pNewEntry;
			}
			if (nNewKey != nOldKey)						// neuer Index
			{
				pNewIndex = new sal_uInt32(nNewKey);
				if (!pMergeTable->Insert(nOldKey,pNewIndex))
					delete pNewIndex;
			}
		}
		else											// benutzerdef.
		{
//			pNewEntry = new SvNumberformat(*pFormat);	// Copy reicht nicht !!!
			pNewEntry = new SvNumberformat( *pFormat, *pFormatScanner );
			nNewKey = ImpIsEntry(pNewEntry->GetFormatstring(),
							  nCLOffset,
							  pFormat->GetLanguage());
			if (nNewKey != NUMBERFORMAT_ENTRY_NOT_FOUND) // schon vorhanden
				delete pNewEntry;
			else
			{
				SvNumberformat* pStdFormat =
						(SvNumberformat*) aFTable.Get(nCLOffset + ZF_STANDARD);
				sal_uInt32 nPos = nCLOffset + pStdFormat->GetLastInsertKey();
				nNewKey = nPos+1;
				if (nPos - nCLOffset >= SV_COUNTRY_LANGUAGE_OFFSET)
				{
					DBG_ERROR(
						"SvNumberFormatter:: Zu viele Formate pro CL");
					delete pNewEntry;
				}
				else if (!aFTable.Insert(nNewKey, pNewEntry))
						delete pNewEntry;
				else
					pStdFormat->SetLastInsertKey((sal_uInt16) (nNewKey - nCLOffset));
			}
			if (nNewKey != nOldKey)						// neuer Index
			{
				pNewIndex = new sal_uInt32(nNewKey);
				if (!pMergeTable->Insert(nOldKey,pNewIndex))
					delete pNewIndex;
			}
		}
		pFormat = rTable.aFTable.Next();
	}
	return pMergeTable;
}


SvNumberFormatterMergeMap SvNumberFormatter::ConvertMergeTableToMap()
{
    if (!HasMergeFmtTbl())
        return SvNumberFormatterMergeMap();

    SvNumberFormatterMergeMap aMap;
    for (sal_uInt32* pIndex = pMergeTable->First(); pIndex; pIndex = pMergeTable->Next())
    {
        sal_uInt32 nOldKey = pMergeTable->GetCurKey();
        aMap.insert( SvNumberFormatterMergeMap::value_type( nOldKey, *pIndex));
    }
    ClearMergeTable();
	return aMap;
}


sal_uInt32 SvNumberFormatter::GetFormatForLanguageIfBuiltIn( sal_uInt32 nFormat,
		LanguageType eLnge )
{
	if ( eLnge == LANGUAGE_DONTKNOW )
        eLnge = IniLnge;
    if ( nFormat < SV_COUNTRY_LANGUAGE_OFFSET && eLnge == IniLnge )
		return nFormat;		// es bleibt wie es ist
	sal_uInt32 nOffset = nFormat % SV_COUNTRY_LANGUAGE_OFFSET;		// relativIndex
	if ( nOffset > SV_MAX_ANZ_STANDARD_FORMATE )
		return nFormat;					// kein eingebautes Format
	sal_uInt32 nCLOffset = ImpGenerateCL(eLnge);		// ggbf. generieren
	return nCLOffset + nOffset;
}


sal_uInt32 SvNumberFormatter::GetFormatIndex( NfIndexTableOffset nTabOff,
		LanguageType eLnge )
{
	if ( nTabOff >= NF_INDEX_TABLE_ENTRIES
			|| theIndexTable[nTabOff] == NUMBERFORMAT_ENTRY_NOT_FOUND )
		return NUMBERFORMAT_ENTRY_NOT_FOUND;
	if ( eLnge == LANGUAGE_DONTKNOW )
        eLnge = IniLnge;
	sal_uInt32 nCLOffset = ImpGenerateCL(eLnge);		// ggbf. generieren
	return nCLOffset + theIndexTable[nTabOff];
}


NfIndexTableOffset SvNumberFormatter::GetIndexTableOffset( sal_uInt32 nFormat ) const
{
	sal_uInt32 nOffset = nFormat % SV_COUNTRY_LANGUAGE_OFFSET;		// relativIndex
	if ( nOffset > SV_MAX_ANZ_STANDARD_FORMATE )
		return NF_INDEX_TABLE_ENTRIES;		// kein eingebautes Format
	for ( sal_uInt16 j = 0; j < NF_INDEX_TABLE_ENTRIES; j++ )
	{
		if ( theIndexTable[j] == nOffset )
			return (NfIndexTableOffset) j;
	}
	return NF_INDEX_TABLE_ENTRIES;		// bad luck
}


void SvNumberFormatter::SetYear2000( sal_uInt16 nVal )
{
	pStringScanner->SetYear2000( nVal );
}


sal_uInt16 SvNumberFormatter::GetYear2000() const
{
	return pStringScanner->GetYear2000();
}


sal_uInt16 SvNumberFormatter::ExpandTwoDigitYear( sal_uInt16 nYear ) const
{
	if ( nYear < 100 )
		return SvNumberFormatter::ExpandTwoDigitYear( nYear,
			pStringScanner->GetYear2000() );
	return nYear;
}


// static
sal_uInt16 SvNumberFormatter::GetYear2000Default()
{
	return (sal_uInt16) ::utl::MiscCfg().GetYear2000();
}

const String& SvNumberFormatter::GetTrueString(){return pFormatScanner->GetTrueString();}
const String& SvNumberFormatter::GetFalseString(){return pFormatScanner->GetFalseString();}

// static
const NfCurrencyTable& SvNumberFormatter::GetTheCurrencyTable()
{
    ::osl::MutexGuard aGuard( GetMutex() );
	while ( !bCurrencyTableInitialized )
		ImpInitCurrencyTable();
	return theCurrencyTable::get();
}


// static
const NfCurrencyEntry* SvNumberFormatter::MatchSystemCurrency()
{
    // MUST call GetTheCurrencyTable() before accessing nSystemCurrencyPosition
	const NfCurrencyTable& rTable = GetTheCurrencyTable();
	return nSystemCurrencyPosition ? rTable[nSystemCurrencyPosition] : NULL;
}


// static
const NfCurrencyEntry& SvNumberFormatter::GetCurrencyEntry( LanguageType eLang )
{
    if ( eLang == LANGUAGE_SYSTEM )
	{
		const NfCurrencyEntry* pCurr = MatchSystemCurrency();
		return pCurr ? *pCurr : *(GetTheCurrencyTable()[0]);
	}
	else
	{
        eLang = MsLangId::getRealLanguage( eLang );
		const NfCurrencyTable& rTable = GetTheCurrencyTable();
		sal_uInt16 nCount = rTable.Count();
		const NfCurrencyEntryPtr* ppData = rTable.GetData();
		for ( sal_uInt16 j = 0; j < nCount; j++, ppData++ )
		{
			if ( (*ppData)->GetLanguage() == eLang )
				return **ppData;
		}
		return *(rTable[0]);
	}
}


// static
const NfCurrencyEntry* SvNumberFormatter::GetCurrencyEntry(
        const String& rAbbrev, LanguageType eLang )
{
    eLang = MsLangId::getRealLanguage( eLang );
    const NfCurrencyTable& rTable = GetTheCurrencyTable();
    sal_uInt16 nCount = rTable.Count();
    const NfCurrencyEntryPtr* ppData = rTable.GetData();
    for ( sal_uInt16 j = 0; j < nCount; j++, ppData++ )
    {
        if ( (*ppData)->GetLanguage() == eLang &&
                (*ppData)->GetBankSymbol() == rAbbrev )
            return *ppData;
    }
    return NULL;
}


// static
const NfCurrencyEntry* SvNumberFormatter::GetLegacyOnlyCurrencyEntry(
        const String& rSymbol, const String& rAbbrev )
{
	if (!bCurrencyTableInitialized)
        GetTheCurrencyTable();      // just for initialization
    const NfCurrencyTable& rTable = theLegacyOnlyCurrencyTable::get();
    sal_uInt16 nCount = rTable.Count();
    const NfCurrencyEntryPtr* ppData = rTable.GetData();
    for ( sal_uInt16 j = 0; j < nCount; j++, ppData++ )
    {
        if ( (*ppData)->GetSymbol() == rSymbol &&
                (*ppData)->GetBankSymbol() == rAbbrev )
            return *ppData;
    }
    return NULL;
}


// static
IMPL_STATIC_LINK_NOINSTANCE( SvNumberFormatter, CurrencyChangeLink, void*, EMPTYARG )
{
    ::osl::MutexGuard aGuard( GetMutex() );
    String aAbbrev;
    LanguageType eLang = LANGUAGE_SYSTEM;
    SvtSysLocaleOptions().GetCurrencyAbbrevAndLanguage( aAbbrev, eLang );
    SetDefaultSystemCurrency( aAbbrev, eLang );
    return 0;
}


// static
void SvNumberFormatter::SetDefaultSystemCurrency( const String& rAbbrev, LanguageType eLang )
{
    ::osl::MutexGuard aGuard( GetMutex() );
    if ( eLang == LANGUAGE_SYSTEM )
        eLang = SvtSysLocale().GetLanguage();
    const NfCurrencyTable& rTable = GetTheCurrencyTable();
    sal_uInt16 nCount = rTable.Count();
    const NfCurrencyEntryPtr* ppData = rTable.GetData();
    if ( rAbbrev.Len() )
    {
        for ( sal_uInt16 j = 0; j < nCount; j++, ppData++ )
        {
            if ( (*ppData)->GetLanguage() == eLang && (*ppData)->GetBankSymbol() == rAbbrev )
            {
                nSystemCurrencyPosition = j;
                return ;
            }
        }
    }
    else
    {
        for ( sal_uInt16 j = 0; j < nCount; j++, ppData++ )
        {
            if ( (*ppData)->GetLanguage() == eLang )
            {
                nSystemCurrencyPosition = j;
                return ;
            }
        }
    }
    nSystemCurrencyPosition = 0;    // not found => simple SYSTEM
}


void SvNumberFormatter::ResetDefaultSystemCurrency()
{
    nDefaultSystemCurrencyFormat = NUMBERFORMAT_ENTRY_NOT_FOUND;
}


sal_uInt32 SvNumberFormatter::ImpGetDefaultSystemCurrencyFormat()
{
	if ( nDefaultSystemCurrencyFormat == NUMBERFORMAT_ENTRY_NOT_FOUND )
	{
		xub_StrLen nCheck;
		short nType;
		NfWSStringsDtor aCurrList;
		sal_uInt16 nDefault = GetCurrencyFormatStrings( aCurrList,
			GetCurrencyEntry( LANGUAGE_SYSTEM ), sal_False );
		DBG_ASSERT( aCurrList.Count(), "where is the NewCurrency System standard format?!?" );
		// if already loaded or user defined nDefaultSystemCurrencyFormat
		// will be set to the right value
		PutEntry( *aCurrList.GetObject( nDefault ), nCheck, nType,
			nDefaultSystemCurrencyFormat, LANGUAGE_SYSTEM );
		DBG_ASSERT( nCheck == 0, "NewCurrency CheckError" );
		DBG_ASSERT( nDefaultSystemCurrencyFormat != NUMBERFORMAT_ENTRY_NOT_FOUND,
			"nDefaultSystemCurrencyFormat == NUMBERFORMAT_ENTRY_NOT_FOUND" );
	}
	return nDefaultSystemCurrencyFormat;
}


sal_uInt32 SvNumberFormatter::ImpGetDefaultCurrencyFormat()
{
	sal_uInt32 CLOffset = ImpGetCLOffset( ActLnge );
	sal_uInt32 nDefaultCurrencyFormat =
		(sal_uInt32)(sal_uLong) aDefaultFormatKeys.Get( CLOffset + ZF_STANDARD_CURRENCY );
	if ( !nDefaultCurrencyFormat )
		nDefaultCurrencyFormat = NUMBERFORMAT_ENTRY_NOT_FOUND;
	if ( nDefaultCurrencyFormat == NUMBERFORMAT_ENTRY_NOT_FOUND )
	{
		// look for a defined standard
		sal_uInt32 nStopKey = CLOffset + SV_COUNTRY_LANGUAGE_OFFSET;
		sal_uInt32 nKey;
		aFTable.Seek( CLOffset );
		while ( (nKey = aFTable.GetCurKey()) >= CLOffset && nKey < nStopKey )
		{
			const SvNumberformat* pEntry =
				(const SvNumberformat*) aFTable.GetCurObject();
			if ( pEntry->IsStandard() && (pEntry->GetType() & NUMBERFORMAT_CURRENCY) )
			{
				nDefaultCurrencyFormat = nKey;
				break;	// while
			}
			aFTable.Next();
		}

		if ( nDefaultCurrencyFormat == NUMBERFORMAT_ENTRY_NOT_FOUND )
		{	// none found, create one
			xub_StrLen nCheck;
			short nType;
			NfWSStringsDtor aCurrList;
			sal_uInt16 nDefault = GetCurrencyFormatStrings( aCurrList,
				GetCurrencyEntry( ActLnge ), sal_False );
			DBG_ASSERT( aCurrList.Count(), "where is the NewCurrency standard format?" );
			if ( aCurrList.Count() )
			{
				// if already loaded or user defined nDefaultSystemCurrencyFormat
				// will be set to the right value
				PutEntry( *aCurrList.GetObject( nDefault ), nCheck, nType,
					nDefaultCurrencyFormat, ActLnge );
				DBG_ASSERT( nCheck == 0, "NewCurrency CheckError" );
				DBG_ASSERT( nDefaultCurrencyFormat != NUMBERFORMAT_ENTRY_NOT_FOUND,
					"nDefaultCurrencyFormat == NUMBERFORMAT_ENTRY_NOT_FOUND" );
			}
			// old automatic currency format as a last resort
			if ( nDefaultCurrencyFormat == NUMBERFORMAT_ENTRY_NOT_FOUND )
				nDefaultCurrencyFormat = CLOffset + ZF_STANDARD_CURRENCY+3;
			else
			{	// mark as standard so that it is found next time
				SvNumberformat* pEntry = aFTable.Get( nDefaultCurrencyFormat );
				if ( pEntry )
					pEntry->SetStandard();
			}
		}
		aDefaultFormatKeys.Insert( CLOffset + ZF_STANDARD_CURRENCY,
			(void*) nDefaultCurrencyFormat );
	}
	return nDefaultCurrencyFormat;
}


// static
// try to make it inline if possible since this a loop body
// sal_True: continue; sal_False: break loop, if pFoundEntry==NULL dupe found
#ifndef DBG_UTIL
inline
#endif
	sal_Bool SvNumberFormatter::ImpLookupCurrencyEntryLoopBody(
		const NfCurrencyEntry*& pFoundEntry, sal_Bool& bFoundBank,
        const NfCurrencyEntry* pData, sal_uInt16 nPos, const String& rSymbol )
{
	sal_Bool bFound;
	if ( pData->GetSymbol() == rSymbol )
	{
		bFound = sal_True;
		bFoundBank = sal_False;
	}
	else if ( pData->GetBankSymbol() == rSymbol )
	{
		bFound = sal_True;
		bFoundBank = sal_True;
	}
	else
		bFound = sal_False;
	if ( bFound )
	{
		if ( pFoundEntry && pFoundEntry != pData )
		{
			pFoundEntry = NULL;
			return sal_False;	// break loop, not unique
		}
		if ( nPos == 0 )
		{	// first entry is SYSTEM
			pFoundEntry = MatchSystemCurrency();
			if ( pFoundEntry )
				return sal_False;	// break loop
				// even if there are more matching entries
				// this one is probably the one we are looking for
			else
				pFoundEntry = pData;
		}
		else
			pFoundEntry = pData;
	}
	return sal_True;
}


sal_Bool SvNumberFormatter::GetNewCurrencySymbolString( sal_uInt32 nFormat,
			String& rStr, const NfCurrencyEntry** ppEntry /* = NULL */,
			sal_Bool* pBank /* = NULL */ ) const
{
	rStr.Erase();
	if ( ppEntry )
		*ppEntry = NULL;
	if ( pBank )
		*pBank = sal_False;
	SvNumberformat* pFormat = (SvNumberformat*) aFTable.Get( nFormat );
	if ( pFormat )
	{
		String aSymbol, aExtension;
		if ( pFormat->GetNewCurrencySymbol( aSymbol, aExtension ) )
		{
			if ( ppEntry )
			{
				sal_Bool bFoundBank = sal_False;
				// we definitely need an entry matching the format code string
				const NfCurrencyEntry* pFoundEntry = GetCurrencyEntry(
					bFoundBank, aSymbol, aExtension, pFormat->GetLanguage(),
					sal_True );
				if ( pFoundEntry )
				{
					*ppEntry = pFoundEntry;
					if ( pBank )
						*pBank = bFoundBank;
					pFoundEntry->BuildSymbolString( rStr, bFoundBank );
				}
			}
			if ( !rStr.Len() )
			{	// analog zu BuildSymbolString
				rStr  = '[';
				rStr += '$';
				if ( aSymbol.Search( '-' ) != STRING_NOTFOUND ||
						aSymbol.Search( ']' ) != STRING_NOTFOUND )
				{
					rStr += '"';
					rStr += aSymbol;
					rStr += '"';
				}
				else
					rStr += aSymbol;
				if ( aExtension.Len() )
					rStr += aExtension;
				rStr += ']';
			}
			return sal_True;
		}
	}
	return sal_False;
}


// static
const NfCurrencyEntry* SvNumberFormatter::GetCurrencyEntry( sal_Bool & bFoundBank,
			const String& rSymbol, const String& rExtension,
            LanguageType eFormatLanguage, sal_Bool bOnlyStringLanguage )
{
	xub_StrLen nExtLen = rExtension.Len();
	LanguageType eExtLang;
	if ( nExtLen )
	{
		sal_Int32 nExtLang = ::rtl::OUString( rExtension ).toInt32( 16 );
		if ( !nExtLang )
			eExtLang = LANGUAGE_DONTKNOW;
		else
			eExtLang = (LanguageType) ((nExtLang < 0) ?
				-nExtLang : nExtLang);
	}
	else
		eExtLang = LANGUAGE_DONTKNOW;
	const NfCurrencyEntry* pFoundEntry = NULL;
	const NfCurrencyTable& rTable = GetTheCurrencyTable();
	sal_uInt16 nCount = rTable.Count();
	sal_Bool bCont = sal_True;

	// first try with given extension language/country
	if ( nExtLen )
	{
		const NfCurrencyEntryPtr* ppData = rTable.GetData();
		for ( sal_uInt16 j = 0; j < nCount && bCont; j++, ppData++ )
		{
			LanguageType eLang = (*ppData)->GetLanguage();
			if ( eLang == eExtLang ||
					((eExtLang == LANGUAGE_DONTKNOW) &&
					(eLang == LANGUAGE_SYSTEM))
				)
			{
				bCont = ImpLookupCurrencyEntryLoopBody( pFoundEntry, bFoundBank,
					*ppData, j, rSymbol );
			}
		}
	}

	// ok?
	if ( pFoundEntry || !bCont || (bOnlyStringLanguage && nExtLen) )
		return pFoundEntry;

	if ( !bOnlyStringLanguage )
	{
		// now try the language/country of the number format
		const NfCurrencyEntryPtr* ppData = rTable.GetData();
		for ( sal_uInt16 j = 0; j < nCount && bCont; j++, ppData++ )
		{
			LanguageType eLang = (*ppData)->GetLanguage();
			if ( eLang == eFormatLanguage ||
					((eFormatLanguage == LANGUAGE_DONTKNOW) &&
					(eLang == LANGUAGE_SYSTEM))
				)
			{
				bCont = ImpLookupCurrencyEntryLoopBody( pFoundEntry, bFoundBank,
					*ppData, j, rSymbol );
			}
		}

		// ok?
		if ( pFoundEntry || !bCont )
			return pFoundEntry;
	}

    // then try without language/country if no extension specified
    if ( !nExtLen )
	{
		const NfCurrencyEntryPtr* ppData = rTable.GetData();
		for ( sal_uInt16 j = 0; j < nCount && bCont; j++, ppData++ )
		{
			bCont = ImpLookupCurrencyEntryLoopBody( pFoundEntry, bFoundBank,
				*ppData, j, rSymbol );
		}
	}

	return pFoundEntry;
}


void SvNumberFormatter::GetCompatibilityCurrency( String& rSymbol, String& rAbbrev ) const
{
    ::com::sun::star::uno::Sequence< ::com::sun::star::i18n::Currency2 >
        xCurrencies = xLocaleData->getAllCurrencies();
    sal_Int32 nCurrencies = xCurrencies.getLength();
    sal_Int32 j;
    for ( j=0; j < nCurrencies; ++j )
    {
        if ( xCurrencies[j].UsedInCompatibleFormatCodes )
        {
            rSymbol = xCurrencies[j].Symbol;
            rAbbrev = xCurrencies[j].BankSymbol;
            break;
        }
    }
    if ( j >= nCurrencies )
    {
        if (LocaleDataWrapper::areChecksEnabled())
        {
            String aMsg( RTL_CONSTASCII_USTRINGPARAM(
                        "GetCompatibilityCurrency: none?"));
            LocaleDataWrapper::outputCheckMessage(
                    xLocaleData->appendLocaleInfo( aMsg));
        }
        rSymbol = xLocaleData->getCurrSymbol();
        rAbbrev = xLocaleData->getCurrBankSymbol();
    }
}


void lcl_CheckCurrencySymbolPosition( const NfCurrencyEntry& rCurr )
{
	short nPos = -1;		// -1:=unknown, 0:=vorne, 1:=hinten
	short nNeg = -1;
	switch ( rCurr.GetPositiveFormat() )
	{
		case 0:                                        	// $1
			nPos = 0;
		break;
		case 1:											// 1$
			nPos = 1;
		break;
		case 2:											// $ 1
			nPos = 0;
		break;
		case 3:											// 1 $
			nPos = 1;
		break;
		default:
			LocaleDataWrapper::outputCheckMessage(
                    "lcl_CheckCurrencySymbolPosition: unknown PositiveFormat");
		break;
	}
	switch ( rCurr.GetNegativeFormat() )
	{
		case 0:                                        	// ($1)
			nNeg = 0;
		break;
		case 1:                                        	// -$1
			nNeg = 0;
		break;
		case 2:                                        	// $-1
			nNeg = 0;
		break;
		case 3:                                        	// $1-
			nNeg = 0;
		break;
		case 4:                                        	// (1$)
			nNeg = 1;
		break;
		case 5:                                        	// -1$
			nNeg = 1;
		break;
		case 6:                                        	// 1-$
			nNeg = 1;
		break;
		case 7:                                        	// 1$-
			nNeg = 1;
		break;
		case 8:                                        	// -1 $
			nNeg = 1;
		break;
		case 9:                                        	// -$ 1
			nNeg = 0;
		break;
		case 10:                                        // 1 $-
			nNeg = 1;
		break;
		case 11:                                        // $ -1
			nNeg = 0;
		break;
		case 12 : 										// $ 1-
			nNeg = 0;
		break;
		case 13 : 										// 1- $
			nNeg = 1;
		break;
		case 14 : 										// ($ 1)
			nNeg = 0;
		break;
		case 15 :										// (1 $)
			nNeg = 1;
		break;
		default:
			LocaleDataWrapper::outputCheckMessage(
                    "lcl_CheckCurrencySymbolPosition: unknown NegativeFormat");
		break;
	}
	if ( nPos >= 0 && nNeg >= 0 && nPos != nNeg )
	{
		ByteString aStr( "positions of currency symbols differ\nLanguage: " );
		aStr += ByteString::CreateFromInt32( rCurr.GetLanguage() );
		aStr += " <";
		aStr += ByteString( rCurr.GetSymbol(), RTL_TEXTENCODING_UTF8 );
		aStr += "> positive: ";
		aStr += ByteString::CreateFromInt32( rCurr.GetPositiveFormat() );
		aStr += ( nPos ? " (postfix)" : " (prefix)" );
		aStr += ", negative: ";
		aStr += ByteString::CreateFromInt32( rCurr.GetNegativeFormat() );
		aStr += ( nNeg ? " (postfix)" : " (prefix)" );
#if 0
// seems that there really are some currencies which differ, e.g. YugoDinar
		DBG_ERRORFILE( aStr.GetBuffer() );
#endif
	}
}


// static
void SvNumberFormatter::ImpInitCurrencyTable()
{
	// racing condition possible:
    // ::osl::MutexGuard aGuard( GetMutex() );
	// while ( !bCurrencyTableInitialized )
	// 		ImpInitCurrencyTable();
    static sal_Bool bInitializing = sal_False;
    if ( bCurrencyTableInitialized || bInitializing )
		return ;
    bInitializing = sal_True;

    RTL_LOGFILE_CONTEXT_AUTHOR( aTimeLog, "svl", "er93726", "SvNumberFormatter::ImpInitCurrencyTable" );

    LanguageType eSysLang = SvtSysLocale().GetLanguage();
    LocaleDataWrapper* pLocaleData = new LocaleDataWrapper(
        ::comphelper::getProcessServiceFactory(),
        MsLangId::convertLanguageToLocale( eSysLang ) );
    // get user configured currency
    String aConfiguredCurrencyAbbrev;
    LanguageType eConfiguredCurrencyLanguage = LANGUAGE_SYSTEM;
    SvtSysLocaleOptions().GetCurrencyAbbrevAndLanguage(
        aConfiguredCurrencyAbbrev, eConfiguredCurrencyLanguage );
    sal_uInt16 nSecondarySystemCurrencyPosition = 0;
    sal_uInt16 nMatchingSystemCurrencyPosition = 0;
	NfCurrencyEntryPtr pEntry;

	// first entry is SYSTEM
    pEntry = new NfCurrencyEntry( *pLocaleData, LANGUAGE_SYSTEM );
	theCurrencyTable::get().Insert( pEntry, 0 );
    sal_uInt16 nCurrencyPos = 1;

	::com::sun::star::uno::Sequence< ::com::sun::star::lang::Locale > xLoc =
		LocaleDataWrapper::getInstalledLocaleNames();
	sal_Int32 nLocaleCount = xLoc.getLength();
    RTL_LOGFILE_CONTEXT_TRACE1( aTimeLog, "number of locales: %ld", nLocaleCount );
    Locale const * const pLocales = xLoc.getConstArray();
    NfCurrencyTable &rCurrencyTable = theCurrencyTable::get();
    NfCurrencyTable &rLegacyOnlyCurrencyTable = theLegacyOnlyCurrencyTable::get();
    sal_uInt16 nLegacyOnlyCurrencyPos = 0;
	for ( sal_Int32 nLocale = 0; nLocale < nLocaleCount; nLocale++ )
	{
        LanguageType eLang = MsLangId::convertLocaleToLanguage(
                pLocales[nLocale]);
#if OSL_DEBUG_LEVEL > 1
		LanguageType eReal = MsLangId::getRealLanguage( eLang );
		if ( eReal != eLang ) {
			sal_Bool bBreak;
            bBreak = sal_True;
        }
#endif
        pLocaleData->setLocale( pLocales[nLocale] );
        Sequence< Currency2 > aCurrSeq = pLocaleData->getAllCurrencies();
		sal_Int32 nCurrencyCount = aCurrSeq.getLength();
        Currency2 const * const pCurrencies = aCurrSeq.getConstArray();

        // one default currency for each locale, insert first so it is found first
        sal_Int32 nDefault;
        for ( nDefault = 0; nDefault < nCurrencyCount; nDefault++ )
        {
            if ( pCurrencies[nDefault].Default )
                break;
        }
        if ( nDefault < nCurrencyCount )
            pEntry = new NfCurrencyEntry( pCurrencies[nDefault], *pLocaleData, eLang );
        else
            pEntry = new NfCurrencyEntry( *pLocaleData, eLang );    // first or ShellsAndPebbles

        if (LocaleDataWrapper::areChecksEnabled())
            lcl_CheckCurrencySymbolPosition( *pEntry );

        rCurrencyTable.Insert( pEntry, nCurrencyPos++ );
        if ( !nSystemCurrencyPosition && (aConfiguredCurrencyAbbrev.Len() ?
                pEntry->GetBankSymbol() == aConfiguredCurrencyAbbrev &&
                pEntry->GetLanguage() == eConfiguredCurrencyLanguage : sal_False) )
            nSystemCurrencyPosition = nCurrencyPos-1;
        if ( !nMatchingSystemCurrencyPosition &&
                pEntry->GetLanguage() == eSysLang )
            nMatchingSystemCurrencyPosition = nCurrencyPos-1;

        // all remaining currencies for each locale
		if ( nCurrencyCount > 1 )
		{
			sal_Int32 nCurrency;
			for ( nCurrency = 0; nCurrency < nCurrencyCount; nCurrency++ )
			{
                if (pCurrencies[nCurrency].LegacyOnly)
                {
                    pEntry = new NfCurrencyEntry( pCurrencies[nCurrency], *pLocaleData, eLang );
                    rLegacyOnlyCurrencyTable.Insert( pEntry, nLegacyOnlyCurrencyPos++ );
                }
                else if ( nCurrency != nDefault )
                {
                    pEntry = new NfCurrencyEntry( pCurrencies[nCurrency], *pLocaleData, eLang );
                    // no dupes
                    sal_Bool bInsert = sal_True;
                    NfCurrencyEntry const * const * pData = rCurrencyTable.GetData();
                    sal_uInt16 n = rCurrencyTable.Count();
                    pData++;        // skip first SYSTEM entry
                    for ( sal_uInt16 j=1; j<n; j++ )
                    {
                        if ( *(*pData++) == *pEntry )
                        {
                            bInsert = sal_False;
                            break;  // for
                        }
                    }
                    if ( !bInsert )
                        delete pEntry;
                    else
                    {
                        rCurrencyTable.Insert( pEntry, nCurrencyPos++ );
                        if ( !nSecondarySystemCurrencyPosition &&
                                (aConfiguredCurrencyAbbrev.Len() ?
                                pEntry->GetBankSymbol() == aConfiguredCurrencyAbbrev :
                                pEntry->GetLanguage() == eConfiguredCurrencyLanguage) )
                            nSecondarySystemCurrencyPosition = nCurrencyPos-1;
                        if ( !nMatchingSystemCurrencyPosition &&
                                pEntry->GetLanguage() ==  eSysLang )
                            nMatchingSystemCurrencyPosition = nCurrencyPos-1;
                    }
                }
			}
		}
	}
    if ( !nSystemCurrencyPosition )
        nSystemCurrencyPosition = nSecondarySystemCurrencyPosition;
    if ((aConfiguredCurrencyAbbrev.Len() && !nSystemCurrencyPosition) &&
            LocaleDataWrapper::areChecksEnabled())
        LocaleDataWrapper::outputCheckMessage(
                "SvNumberFormatter::ImpInitCurrencyTable: configured currency not in I18N locale data.");
    // match SYSTEM if no configured currency found
    if ( !nSystemCurrencyPosition )
        nSystemCurrencyPosition = nMatchingSystemCurrencyPosition;
    if ((!aConfiguredCurrencyAbbrev.Len() && !nSystemCurrencyPosition) &&
            LocaleDataWrapper::areChecksEnabled())
        LocaleDataWrapper::outputCheckMessage(
                "SvNumberFormatter::ImpInitCurrencyTable: system currency not in I18N locale data.");
    delete pLocaleData;
    SvtSysLocaleOptions::SetCurrencyChangeLink(
        STATIC_LINK( NULL, SvNumberFormatter, CurrencyChangeLink ) );
    bInitializing = sal_False;
	bCurrencyTableInitialized = sal_True;
}


sal_uInt16 SvNumberFormatter::GetCurrencyFormatStrings( NfWSStringsDtor& rStrArr,
			const NfCurrencyEntry& rCurr, sal_Bool bBank ) const
{
	sal_uInt16 nDefault = 0;
	if ( bBank )
	{	// nur Bankensymbole
		String aPositiveBank, aNegativeBank;
        rCurr.BuildPositiveFormatString( aPositiveBank, sal_True, *xLocaleData, 1 );
        rCurr.BuildNegativeFormatString( aNegativeBank, sal_True, *xLocaleData, 1 );

		WSStringPtr pFormat1 = new String( aPositiveBank );
		*pFormat1 += ';';
		WSStringPtr pFormat2 = new String( *pFormat1 );

		String aRed( '[' );
		aRed += pFormatScanner->GetRedString();
		aRed += ']';

		*pFormat2 += aRed;

		*pFormat1 += aNegativeBank;
		*pFormat2 += aNegativeBank;

		rStrArr.Insert( pFormat1, rStrArr.Count() );
		rStrArr.Insert( pFormat2, rStrArr.Count() );
		nDefault = rStrArr.Count() - 1;
	}
	else
	{	// gemischte Formate wie in SvNumberFormatter::ImpGenerateFormats
		// aber keine doppelten, wenn keine Nachkommastellen in Waehrung
		String aPositive, aNegative, aPositiveNoDec, aNegativeNoDec,
			aPositiveDashed, aNegativeDashed;
		WSStringPtr pFormat1, pFormat2, pFormat3, pFormat4, pFormat5;

		String aRed( '[' );
		aRed += pFormatScanner->GetRedString();
		aRed += ']';

        rCurr.BuildPositiveFormatString( aPositive, sal_False, *xLocaleData, 1 );
        rCurr.BuildNegativeFormatString( aNegative, sal_False, *xLocaleData, 1 );
		if ( rCurr.GetDigits() )
		{
            rCurr.BuildPositiveFormatString( aPositiveNoDec, sal_False, *xLocaleData, 0 );
            rCurr.BuildNegativeFormatString( aNegativeNoDec, sal_False, *xLocaleData, 0 );
            rCurr.BuildPositiveFormatString( aPositiveDashed, sal_False, *xLocaleData, 2 );
            rCurr.BuildNegativeFormatString( aNegativeDashed, sal_False, *xLocaleData, 2 );

			pFormat1 = new String( aPositiveNoDec );
			*pFormat1 += ';';
			pFormat3 = new String( *pFormat1 );
			pFormat5 = new String( aPositiveDashed );
			*pFormat5 += ';';

			*pFormat1 += aNegativeNoDec;

			*pFormat3 += aRed;
			*pFormat5 += aRed;

			*pFormat3 += aNegativeNoDec;
			*pFormat5 += aNegativeDashed;
		}
		else
		{
			pFormat1 = NULL;
			pFormat3 = NULL;
			pFormat5 = NULL;
		}

		pFormat2 = new String( aPositive );
		*pFormat2 += ';';
		pFormat4 = new String( *pFormat2 );

		*pFormat2 += aNegative;

		*pFormat4 += aRed;
		*pFormat4 += aNegative;

		if ( pFormat1 )
			rStrArr.Insert( pFormat1, rStrArr.Count() );
		rStrArr.Insert( pFormat2, rStrArr.Count() );
		if ( pFormat3 )
			rStrArr.Insert( pFormat3, rStrArr.Count() );
		rStrArr.Insert( pFormat4, rStrArr.Count() );
		nDefault = rStrArr.Count() - 1;
		if ( pFormat5 )
			rStrArr.Insert( pFormat5, rStrArr.Count() );
	}
	return nDefault;
}


//--- NfCurrencyEntry ----------------------------------------------------

NfCurrencyEntry::NfCurrencyEntry()
	:	eLanguage( LANGUAGE_DONTKNOW ),
		nPositiveFormat(3),
		nNegativeFormat(8),
		nDigits(2),
		cZeroChar('0')
{
}


NfCurrencyEntry::NfCurrencyEntry( const LocaleDataWrapper& rLocaleData, LanguageType eLang )
{
	aSymbol			= rLocaleData.getCurrSymbol();
	aBankSymbol		= rLocaleData.getCurrBankSymbol();
	eLanguage		= eLang;
	nPositiveFormat	= rLocaleData.getCurrPositiveFormat();
	nNegativeFormat	= rLocaleData.getCurrNegativeFormat();
	nDigits			= rLocaleData.getCurrDigits();
	cZeroChar		= rLocaleData.getCurrZeroChar();
}


NfCurrencyEntry::NfCurrencyEntry( const ::com::sun::star::i18n::Currency & rCurr,
			const LocaleDataWrapper& rLocaleData, LanguageType eLang )
{
	aSymbol			= rCurr.Symbol;
	aBankSymbol		= rCurr.BankSymbol;
	eLanguage		= eLang;
	nPositiveFormat	= rLocaleData.getCurrPositiveFormat();
	nNegativeFormat	= rLocaleData.getCurrNegativeFormat();
    nDigits         = rCurr.DecimalPlaces;
	cZeroChar		= rLocaleData.getCurrZeroChar();
}


sal_Bool NfCurrencyEntry::operator==( const NfCurrencyEntry& r ) const
{
	return aSymbol		== r.aSymbol
		&& aBankSymbol	== r.aBankSymbol
		&& eLanguage	== r.eLanguage
		;
}


void NfCurrencyEntry::SetEuro()
{
	aSymbol = NfCurrencyEntry::GetEuroSymbol();
	aBankSymbol.AssignAscii( RTL_CONSTASCII_STRINGPARAM( "EUR" ) );
	eLanguage		= LANGUAGE_DONTKNOW;
	nPositiveFormat	= 3;
	nNegativeFormat	= 8;
	nDigits			= 2;
	cZeroChar		= '0';
}


sal_Bool NfCurrencyEntry::IsEuro() const
{
	if ( aBankSymbol.EqualsAscii( "EUR" ) )
		return sal_True;
	String aEuro( NfCurrencyEntry::GetEuroSymbol() );
	return aSymbol == aEuro;
}


void NfCurrencyEntry::ApplyVariableInformation( const NfCurrencyEntry& r )
{
	nPositiveFormat	= r.nPositiveFormat;
	nNegativeFormat	= r.nNegativeFormat;
	cZeroChar		= r.cZeroChar;
}


void NfCurrencyEntry::BuildSymbolString( String& rStr, sal_Bool bBank,
			sal_Bool bWithoutExtension ) const
{
	rStr  = '[';
	rStr += '$';
	if ( bBank )
		rStr += aBankSymbol;
	else
	{
		if ( aSymbol.Search( '-' ) != STRING_NOTFOUND || aSymbol.Search( ']' ) != STRING_NOTFOUND )
		{
			rStr += '"';
			rStr += aSymbol;
			rStr += '"';
		}
		else
			rStr += aSymbol;
		if ( !bWithoutExtension && eLanguage != LANGUAGE_DONTKNOW && eLanguage != LANGUAGE_SYSTEM )
		{
			rStr += '-';
			rStr += String::CreateFromInt32( sal_Int32( eLanguage ), 16 ).ToUpperAscii();
		}
	}
	rStr += ']';
}


void NfCurrencyEntry::Impl_BuildFormatStringNumChars( String& rStr,
			const LocaleDataWrapper& rLoc, sal_uInt16 nDecimalFormat ) const
{
	rStr.AssignAscii( RTL_CONSTASCII_STRINGPARAM( "###0" ) );
	rStr.Insert( rLoc.getNumThousandSep(), 1 );
	if ( nDecimalFormat && nDigits )
	{
		rStr += rLoc.getNumDecimalSep();
		rStr.Expand( rStr.Len() + nDigits, (nDecimalFormat == 2 ? '-' : cZeroChar) );
	}
}


void NfCurrencyEntry::BuildPositiveFormatString( String& rStr, sal_Bool bBank,
			const LocaleDataWrapper& rLoc, sal_uInt16 nDecimalFormat ) const
{
	Impl_BuildFormatStringNumChars( rStr, rLoc, nDecimalFormat );
	sal_uInt16 nPosiForm = NfCurrencyEntry::GetEffectivePositiveFormat(
		rLoc.getCurrPositiveFormat(), nPositiveFormat, bBank );
	CompletePositiveFormatString( rStr, bBank, nPosiForm );
}


void NfCurrencyEntry::BuildNegativeFormatString( String& rStr, sal_Bool bBank,
			const LocaleDataWrapper& rLoc, sal_uInt16 nDecimalFormat ) const
{
	Impl_BuildFormatStringNumChars( rStr, rLoc, nDecimalFormat );
	sal_uInt16 nNegaForm = NfCurrencyEntry::GetEffectiveNegativeFormat(
		rLoc.getCurrNegativeFormat(), nNegativeFormat, bBank );
	CompleteNegativeFormatString( rStr, bBank, nNegaForm );
}


void NfCurrencyEntry::CompletePositiveFormatString( String& rStr, sal_Bool bBank,
			sal_uInt16 nPosiForm ) const
{
	String aSymStr;
	BuildSymbolString( aSymStr, bBank );
	NfCurrencyEntry::CompletePositiveFormatString( rStr, aSymStr, nPosiForm );
}


void NfCurrencyEntry::CompleteNegativeFormatString( String& rStr, sal_Bool bBank,
			sal_uInt16 nNegaForm ) const
{
	String aSymStr;
	BuildSymbolString( aSymStr, bBank );
	NfCurrencyEntry::CompleteNegativeFormatString( rStr, aSymStr, nNegaForm );
}


// static
void NfCurrencyEntry::CompletePositiveFormatString( String& rStr,
		const String& rSymStr, sal_uInt16 nPositiveFormat )
{
	switch( nPositiveFormat )
	{
		case 0:                                        	// $1
			rStr.Insert( rSymStr , 0 );
		break;
		case 1:											// 1$
			rStr += rSymStr;
		break;
		case 2:											// $ 1
		{
			rStr.Insert( ' ', 0 );
			rStr.Insert( rSymStr, 0 );
		}
		break;
		case 3:                                         // 1 $
		{
			rStr += ' ';
			rStr += rSymStr;
		}
		break;
		default:
			DBG_ERROR("NfCurrencyEntry::CompletePositiveFormatString: unknown option");
		break;
	}
}


// static
void NfCurrencyEntry::CompleteNegativeFormatString( String& rStr,
		const String& rSymStr, sal_uInt16 nNegativeFormat )
{
	switch( nNegativeFormat )
	{
		case 0:                                        	// ($1)
		{
			rStr.Insert( rSymStr, 0);
			rStr.Insert('(',0);
			rStr += ')';
		}
		break;
		case 1:                                        	// -$1
		{
			rStr.Insert( rSymStr, 0);
			rStr.Insert('-',0);
		}
		break;
		case 2:                                        	// $-1
		{
			rStr.Insert('-',0);
			rStr.Insert( rSymStr, 0);
		}
		break;
		case 3:                                        	// $1-
		{
			rStr.Insert( rSymStr, 0);
			rStr += '-';
		}
		break;
		case 4:                                        	// (1$)
		{
			rStr.Insert('(',0);
			rStr += rSymStr;
			rStr += ')';
		}
		break;
		case 5:                                        	// -1$
		{
			rStr += rSymStr;
			rStr.Insert('-',0);
		}
		break;
		case 6:                                        	// 1-$
		{
			rStr += '-';
			rStr += rSymStr;
		}
		break;
		case 7:                                        	// 1$-
		{
			rStr += rSymStr;
			rStr += '-';
		}
		break;
		case 8:                                        	// -1 $
		{
			rStr += ' ';
			rStr += rSymStr;
			rStr.Insert('-',0);
		}
		break;
		case 9:                                        	// -$ 1
		{
			rStr.Insert(' ',0);
			rStr.Insert( rSymStr, 0);
			rStr.Insert('-',0);
		}
		break;
		case 10:                                        // 1 $-
		{
			rStr += ' ';
			rStr += rSymStr;
			rStr += '-';
		}
		break;
		case 11:                                        // $ -1
		{
			String aTmp( rSymStr );
			aTmp += ' ';
			aTmp += '-';
			rStr.Insert( aTmp, 0 );
		}
		break;
		case 12 : 										// $ 1-
		{
			rStr.Insert(' ', 0);
			rStr.Insert( rSymStr, 0);
			rStr += '-';
		}
		break;
		case 13 : 										// 1- $
		{
			rStr += '-';
			rStr += ' ';
			rStr += rSymStr;
		}
		break;
		case 14 : 										// ($ 1)
		{
			rStr.Insert(' ',0);
			rStr.Insert( rSymStr, 0);
			rStr.Insert('(',0);
			rStr += ')';
		}
		break;
		case 15 :										// (1 $)
		{
			rStr.Insert('(',0);
			rStr += ' ';
			rStr += rSymStr;
			rStr += ')';
		}
		break;
		default:
			DBG_ERROR("NfCurrencyEntry::CompleteNegativeFormatString: unknown option");
		break;
	}
}


// static
sal_uInt16 NfCurrencyEntry::GetEffectivePositiveFormat( sal_uInt16
#if ! NF_BANKSYMBOL_FIX_POSITION
            nIntlFormat
#endif
            , sal_uInt16 nCurrFormat, sal_Bool bBank )
{
	if ( bBank )
	{
#if NF_BANKSYMBOL_FIX_POSITION
		return 3;
#else
		switch ( nIntlFormat )
		{
			case 0:                                        	// $1
				nIntlFormat = 2;                            // $ 1
			break;
			case 1:											// 1$
				nIntlFormat = 3;                            // 1 $
			break;
			case 2:											// $ 1
			break;
			case 3:                                         // 1 $
			break;
			default:
				DBG_ERROR("NfCurrencyEntry::GetEffectivePositiveFormat: unknown option");
			break;
		}
		return nIntlFormat;
#endif
	}
	else
		return nCurrFormat;
}


// nur aufrufen, wenn nCurrFormat wirklich mit Klammern ist
sal_uInt16 lcl_MergeNegativeParenthesisFormat( sal_uInt16 nIntlFormat, sal_uInt16 nCurrFormat )
{
	short nSign = 0;		// -1:=Klammer 0:=links, 1:=mitte, 2:=rechts
	switch ( nIntlFormat )
	{
		case 0:                                        	// ($1)
		case 4:                                        	// (1$)
		case 14 : 										// ($ 1)
		case 15 :										// (1 $)
			return nCurrFormat;
		case 1:                                        	// -$1
		case 5:                                        	// -1$
		case 8:                                        	// -1 $
		case 9:                                        	// -$ 1
			nSign = 0;
		break;
		case 2:                                        	// $-1
		case 6:                                        	// 1-$
		case 11 : 										// $ -1
		case 13 : 										// 1- $
			nSign = 1;
		break;
		case 3:                                        	// $1-
		case 7:                                        	// 1$-
		case 10:                                        // 1 $-
		case 12 : 										// $ 1-
			nSign = 2;
		break;
		default:
			DBG_ERROR("lcl_MergeNegativeParenthesisFormat: unknown option");
		break;
	}

	switch ( nCurrFormat )
	{
		case 0:                                        	// ($1)
			switch ( nSign )
			{
				case 0:
					return 1;                           // -$1
				case 1:
					return 2;                           // $-1
				case 2:
					return 3;                           // $1-
			}
		break;
		case 4:                                        	// (1$)
			switch ( nSign )
			{
				case 0:
					return 5;                           // -1$
				case 1:
					return 6;                           // 1-$
				case 2:
					return 7;                           // 1$-
			}
		break;
		case 14 : 										// ($ 1)
			switch ( nSign )
			{
				case 0:
					return 9;                           // -$ 1
				case 1:
					return 11;                          // $ -1
				case 2:
					return 12;                          // $ 1-
			}
		break;
		case 15 :										// (1 $)
			switch ( nSign )
			{
				case 0:
					return 8;                           // -1 $
				case 1:
					return 13;                          // 1- $
				case 2:
					return 10;                          // 1 $-
			}
		break;
	}
	return nCurrFormat;
}


// static
sal_uInt16 NfCurrencyEntry::GetEffectiveNegativeFormat( sal_uInt16 nIntlFormat,
			sal_uInt16 nCurrFormat, sal_Bool bBank )
{
	if ( bBank )
	{
#if NF_BANKSYMBOL_FIX_POSITION
		return 8;
#else
		switch ( nIntlFormat )
		{
			case 0:                                        	// ($1)
//				nIntlFormat = 14;                           // ($ 1)
				nIntlFormat = 9;                            // -$ 1
			break;
			case 1:                                        	// -$1
				nIntlFormat = 9;                            // -$ 1
			break;
			case 2:                                        	// $-1
				nIntlFormat = 11;                           // $ -1
			break;
			case 3:                                        	// $1-
				nIntlFormat = 12;                           // $ 1-
			break;
			case 4:                                        	// (1$)
//				nIntlFormat = 15;                           // (1 $)
				nIntlFormat = 8;                            // -1 $
			break;
			case 5:                                        	// -1$
				nIntlFormat = 8;                            // -1 $
			break;
			case 6:                                        	// 1-$
				nIntlFormat = 13;                           // 1- $
			break;
			case 7:                                        	// 1$-
				nIntlFormat = 10;                           // 1 $-
			break;
			case 8:                                        	// -1 $
			break;
			case 9:                                        	// -$ 1
			break;
			case 10:                                        // 1 $-
			break;
			case 11:                                        // $ -1
			break;
			case 12 : 										// $ 1-
			break;
			case 13 : 										// 1- $
			break;
			case 14 : 										// ($ 1)
//				nIntlFormat = 14;                           // ($ 1)
				nIntlFormat = 9;                            // -$ 1
			break;
			case 15 :										// (1 $)
//				nIntlFormat = 15;                           // (1 $)
				nIntlFormat = 8;                            // -1 $
			break;
			default:
				DBG_ERROR("NfCurrencyEntry::GetEffectiveNegativeFormat: unknown option");
			break;
		}
#endif
	}
	else if ( nIntlFormat != nCurrFormat )
	{
		switch ( nCurrFormat )
		{
			case 0:                                        	// ($1)
				nIntlFormat = lcl_MergeNegativeParenthesisFormat(
					nIntlFormat, nCurrFormat );
			break;
			case 1:                                        	// -$1
				nIntlFormat = nCurrFormat;
			break;
			case 2:                                        	// $-1
				nIntlFormat = nCurrFormat;
			break;
			case 3:                                        	// $1-
				nIntlFormat = nCurrFormat;
			break;
			case 4:                                        	// (1$)
				nIntlFormat = lcl_MergeNegativeParenthesisFormat(
					nIntlFormat, nCurrFormat );
			break;
			case 5:                                        	// -1$
				nIntlFormat = nCurrFormat;
			break;
			case 6:                                        	// 1-$
				nIntlFormat = nCurrFormat;
			break;
			case 7:                                        	// 1$-
				nIntlFormat = nCurrFormat;
			break;
			case 8:                                        	// -1 $
				nIntlFormat = nCurrFormat;
			break;
			case 9:                                        	// -$ 1
				nIntlFormat = nCurrFormat;
			break;
			case 10:                                        // 1 $-
				nIntlFormat = nCurrFormat;
			break;
			case 11:                                        // $ -1
				nIntlFormat = nCurrFormat;
			break;
			case 12 : 										// $ 1-
				nIntlFormat = nCurrFormat;
			break;
			case 13 : 										// 1- $
				nIntlFormat = nCurrFormat;
			break;
			case 14 : 										// ($ 1)
				nIntlFormat = lcl_MergeNegativeParenthesisFormat(
					nIntlFormat, nCurrFormat );
			break;
			case 15 :										// (1 $)
				nIntlFormat = lcl_MergeNegativeParenthesisFormat(
					nIntlFormat, nCurrFormat );
			break;
			default:
				DBG_ERROR("NfCurrencyEntry::GetEffectiveNegativeFormat: unknown option");
			break;
		}
	}
	return nIntlFormat;
}


// we only support default encodings here
// static
sal_Char NfCurrencyEntry::GetEuroSymbol( rtl_TextEncoding eTextEncoding )
{
	switch ( eTextEncoding )
	{
		case RTL_TEXTENCODING_MS_1252 :			// WNT Ansi
		case RTL_TEXTENCODING_ISO_8859_1 :		// UNX for use with TrueType fonts
			return '\x80';
		case RTL_TEXTENCODING_ISO_8859_15 :		// UNX real
			return '\xA4';
		case RTL_TEXTENCODING_IBM_850 :			// OS2
			return '\xD5';
		case RTL_TEXTENCODING_APPLE_ROMAN :		// MAC
			return '\xDB';
		default:								// default system
#if WNT
			return '\x80';
#elif OS2
			return '\xD5';
#elif UNX
//			return '\xA4';		// #56121# 0xA4 wäre korrekt für iso-8859-15
			return '\x80';		// aber Windows-Code für die konvertierten TrueType-Fonts
#else
#error EuroSymbol is what?
			return '\x80';
#endif
	}
	return '\x80';
}

/* vim: set noet sw=4 ts=4: */
