/**
 * @file llfilepicker.cpp
 * @brief OS-specific file picker
 *
 * $LicenseInfo:firstyear=2001&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "llfilepicker.h"
#include "llworld.h"
#include "llviewerwindow.h"
#include "llkeyboard.h"
#include "lldir.h"
#include "llframetimer.h"
#include "lltrans.h"
#include "llviewercontrol.h"
#include "llwindow.h"   // beforeDialog()

#undef LL_GTK
#if LL_SDL
#include "llwindowsdl.h" // for some X/GTK utils to help with filepickers
#endif // LL_SDL

#ifdef LL_FLTK
  #include "FL/Fl.H"
  #include "FL/Fl_Native_File_Chooser.H"
  #if LL_GLIB
    #include <gio/gio.h> // xdg-desktop-portal file chooser
  #endif
#endif

#if LL_LINUX
#include "llhttpconstants.h"    // file picker uses some of thes constants on Linux
#endif

//
// Globals
//

LLFilePicker LLFilePicker::sInstance;

#if LL_WINDOWS
#define SOUND_FILTER L"Sounds (*.wav)\0*.wav\0"
#define IMAGE_FILTER L"Images (*.tga; *.bmp; *.jpg; *.jpeg; *.png)\0*.tga;*.bmp;*.jpg;*.jpeg;*.png\0"
#define ANIM_FILTER L"Animations (*.bvh; *.anim)\0*.bvh;*.anim\0"
#define COLLADA_FILTER L"Scene (*.dae)\0*.dae\0"
#define GLTF_FILTER L"glTF (*.gltf; *.glb)\0*.gltf;*.glb\0"
#define XML_FILTER L"XML files (*.xml)\0*.xml\0"
#define SLOBJECT_FILTER L"Objects (*.slobject)\0*.slobject\0"
#define RAW_FILTER L"RAW files (*.raw)\0*.raw\0"
#define MODEL_FILTER L"Model files (*.dae, *.gltf, *.glb)\0*.dae;*.gltf;*.glb\0"
#define MATERIAL_FILTER L"GLTF Files (*.gltf; *.glb)\0*.gltf;*.glb\0"
#define HDRI_FILTER L"HDRI Files (*.exr)\0*.exr\0"
#define MATERIAL_TEXTURES_FILTER L"GLTF Import (*.gltf; *.glb; *.tga; *.bmp; *.jpg; *.jpeg; *.png)\0*.gltf;*.glb;*.tga;*.bmp;*.jpg;*.jpeg;*.png\0"
#define SCRIPT_FILTER L"Script files (*.lsl)\0*.lsl\0"
#define DICTIONARY_FILTER L"Dictionary files (*.dic; *.xcu)\0*.dic;*.xcu\0"
// <FS:CR> Import filter
//#define IMPORT_FILTER L"Import (*.oxp; *.hpa)\0*.oxp;*.hpa\0"
#define IMPORT_FILTER L"Import (*.oxp)\0*.oxp\0"
// </FS:CR>
#define EXE_FILTER L"Programs (*.exe)\0*.exe\0" // <FS:LO> fix file picker EXE filtering
#endif

#ifdef LL_DARWIN
#include "llfilepicker_mac.h"
//#include <boost/algorithm/string/predicate.hpp>
#endif

//
// Implementation
//
LLFilePicker::LLFilePicker()
    : mCurrentFile(0),
      mLocked(false)

{
    reset();

#if LL_WINDOWS
    mOFN.lStructSize = sizeof(OPENFILENAMEW);
    mOFN.hwndOwner = NULL;  // Set later
    mOFN.hInstance = NULL;
    mOFN.lpstrCustomFilter = NULL;
    mOFN.nMaxCustFilter = 0;
    mOFN.lpstrFile = NULL;                          // set in open and close
    mOFN.nMaxFile = LL_MAX_PATH;
    mOFN.lpstrFileTitle = NULL;
    mOFN.nMaxFileTitle = 0;
    mOFN.lpstrInitialDir = NULL;
    mOFN.lpstrTitle = NULL;
    mOFN.Flags = 0;                                 // set in open and close
    mOFN.nFileOffset = 0;
    mOFN.nFileExtension = 0;
    mOFN.lpstrDefExt = NULL;
    mOFN.lCustData = 0L;
    mOFN.lpfnHook = NULL;
    mOFN.lpTemplateName = NULL;
    mFilesW[0] = '\0';
#elif LL_DARWIN
    mPickOptions = 0;
#endif

}

LLFilePicker::~LLFilePicker()
{
    // nothing
}

// utility function to check if access to local file system via file browser
// is enabled and if not, tidy up and indicate we're not allowed to do this.
bool LLFilePicker::check_local_file_access_enabled()
{
    // if local file browsing is turned off, return without opening dialog
    bool local_file_system_browsing_enabled = gSavedSettings.getBOOL("LocalFileSystemBrowsingEnabled");
    if ( ! local_file_system_browsing_enabled )
    {
        mFiles.clear();
        return false;
    }

    return true;
}

const std::string LLFilePicker::getFirstFile()
{
    mCurrentFile = 0;
    return getNextFile();
}

const std::string LLFilePicker::getNextFile()
{
    if (mCurrentFile >= getFileCount())
    {
        mLocked = false;
        return std::string();
    }
    else
    {
        return mFiles[mCurrentFile++];
    }
}

const std::string LLFilePicker::getCurFile()
{
    if (mCurrentFile >= getFileCount())
    {
        mLocked = false;
        return std::string();
    }
    else
    {
        return mFiles[mCurrentFile];
    }
}

void LLFilePicker::reset()
{
    mLocked = false;
    mFiles.clear();
    mCurrentFile = 0;
}

#if LL_WINDOWS

bool LLFilePicker::setupFilter(ELoadFilter filter)
{
    bool res = true;
    switch (filter)
    {
    case FFLOAD_ALL:
        // <FS:LO> fix file picker EXE filtering
        mOFN.lpstrFilter = L"All Files (*.*)\0*.*\0" \
        SOUND_FILTER \
        IMAGE_FILTER \
        ANIM_FILTER \
        MATERIAL_FILTER \
        L"\0";
        break;
    case FFLOAD_EXE:
        // <FS:LO> fix file picker EXE filtering
        /*mOFN.lpstrFilter = L"All Files (*.*)\0*.*\0" \
        SOUND_FILTER \
        IMAGE_FILTER \
        ANIM_FILTER \*/
        mOFN.lpstrFilter = EXE_FILTER \
            L"\0";
        break;
    case FFLOAD_WAV:
        mOFN.lpstrFilter = SOUND_FILTER \
            L"\0";
        break;
    case FFLOAD_IMAGE:
        mOFN.lpstrFilter = IMAGE_FILTER \
            L"\0";
        break;
    case FFLOAD_ANIM:
        mOFN.lpstrFilter = ANIM_FILTER \
            L"\0";
        break;
    case FFLOAD_GLTF:
        mOFN.lpstrFilter = GLTF_FILTER \
            L"\0";
        break;
    case FFLOAD_COLLADA:
        mOFN.lpstrFilter = COLLADA_FILTER \
            L"\0";
        break;
    case FFLOAD_XML:
        mOFN.lpstrFilter = XML_FILTER \
            L"\0";
        break;
    case FFLOAD_SLOBJECT:
        mOFN.lpstrFilter = SLOBJECT_FILTER \
            L"\0";
        break;
    case FFLOAD_RAW:
        mOFN.lpstrFilter = RAW_FILTER \
            L"\0";
        break;
    case FFLOAD_MODEL:
        mOFN.lpstrFilter = MODEL_FILTER \
            COLLADA_FILTER \
            MATERIAL_FILTER \
            L"\0";
        break;
    case FFLOAD_MATERIAL:
        mOFN.lpstrFilter = MATERIAL_FILTER \
            L"\0";
        break;
    case FFLOAD_MATERIAL_TEXTURE:
        mOFN.lpstrFilter = MATERIAL_TEXTURES_FILTER \
            MATERIAL_FILTER \
            IMAGE_FILTER \
            L"\0";
        break;
    case FFLOAD_HDRI:
        mOFN.lpstrFilter = HDRI_FILTER \
            L"\0";
        break;
    case FFLOAD_SCRIPT:
        mOFN.lpstrFilter = SCRIPT_FILTER \
            L"\0";
        break;
    case FFLOAD_DICTIONARY:
        mOFN.lpstrFilter = DICTIONARY_FILTER \
            L"\0";
        break;
// <FS:CR> Import filter
    case FFLOAD_IMPORT:
        mOFN.lpstrFilter = IMPORT_FILTER \
            L"\0";
        break;
// </FS:CR>
    default:
        res = false;
        break;
    }
    return res;
}

bool LLFilePicker::getOpenFile(ELoadFilter filter, bool blocking)
{
    if (mLocked)
    {
        return false;
    }
    bool success = false;

    // if local file browsing is turned off, return without opening dialog
    if (!check_local_file_access_enabled())
    {
        return false;
    }

    // don't provide default file selection
    mFilesW[0] = '\0';

    mOFN.hwndOwner = (HWND)gViewerWindow->getPlatformWindow();
    mOFN.lpstrFile = mFilesW;
    mOFN.nMaxFile = SINGLE_FILENAME_BUFFER_SIZE;
    mOFN.Flags = OFN_HIDEREADONLY | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR ;
    mOFN.nFilterIndex = 1;

    setupFilter(filter);

    if (blocking)
    {
        // Modal, so pause agent
        send_agent_pause();
    }

    reset();

    // NOTA BENE: hitting the file dialog triggers a window focus event, destroying the selection manager!!
    success = GetOpenFileName(&mOFN);
    if (success)
    {
        std::string filename = ll_convert<std::string>(std::wstring(mFilesW));
        mFiles.push_back(filename);
    }

    if (blocking)
    {
        send_agent_resume();
        // Account for the fact that the app has been stalled.
        LLFrameTimer::updateFrameTime();
    }

    return success;
}

bool LLFilePicker::getOpenFileModeless(ELoadFilter filter,
                                       void (*callback)(bool, std::vector<std::string> &, void*),
                                       void *userdata)
{
    // not supposed to be used yet, use LLFilePickerThread
    LL_ERRS() << "NOT IMPLEMENTED" << LL_ENDL;
    return false;
}

bool LLFilePicker::getMultipleOpenFiles(ELoadFilter filter, bool blocking)
{
    if( mLocked )
    {
        return false;
    }
    bool success = false;

    // if local file browsing is turned off, return without opening dialog
    if (!check_local_file_access_enabled())
    {
        return false;
    }

    // don't provide default file selection
    mFilesW[0] = '\0';

    mOFN.hwndOwner = (HWND)gViewerWindow->getPlatformWindow();
    mOFN.lpstrFile = mFilesW;
    mOFN.nFilterIndex = 1;
    mOFN.nMaxFile = FILENAME_BUFFER_SIZE;
    mOFN.Flags = OFN_HIDEREADONLY | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR |
        OFN_EXPLORER | OFN_ALLOWMULTISELECT;

    setupFilter(filter);

    reset();

    if (blocking)
    {
        // Modal, so pause agent
        send_agent_pause();
    }

    // NOTA BENE: hitting the file dialog triggers a window focus event, destroying the selection manager!!
    success = GetOpenFileName(&mOFN); // pauses until ok or cancel.
    if( success )
    {
        // The getopenfilename api doesn't tell us if we got more than
        // one file, so we have to test manually by checking string
        // lengths.
        if( wcslen(mOFN.lpstrFile) > mOFN.nFileOffset ) /*Flawfinder: ignore*/
        {
            std::string filename = ll_convert<std::string>(std::wstring(mFilesW));
            mFiles.push_back(filename);
        }
        else
        {
            mLocked = true;
            WCHAR* tptrw = mFilesW;
            std::string dirname;
            while(1)
            {
                if (*tptrw == 0 && *(tptrw+1) == 0) // double '\0'
                    break;
                if (*tptrw == 0)
                    tptrw++; // shouldn't happen?
                std::string filename = ll_convert<std::string>(std::wstring(tptrw));
                if (dirname.empty())
                    dirname = filename + "\\";
                else
                    mFiles.push_back(dirname + filename);
                tptrw += wcslen(tptrw);
            }
        }
    }

    if (blocking)
    {
        send_agent_resume();
    }

    // Account for the fact that the app has been stalled.
    LLFrameTimer::updateFrameTime();
    return success;
}

bool LLFilePicker::getMultipleOpenFilesModeless(ELoadFilter filter,
                                                void (*callback)(bool, std::vector<std::string> &, void*),
                                                void *userdata )
{
    // not supposed to be used yet, use LLFilePickerThread
    LL_ERRS() << "NOT IMPLEMENTED" << LL_ENDL;
    return false;
}

bool LLFilePicker::getSaveFile(ESaveFilter filter, const std::string& filename, bool blocking)
{
    if( mLocked )
    {
        return false;
    }
    bool success = false;

    // if local file browsing is turned off, return without opening dialog
    if (!check_local_file_access_enabled())
    {
        return false;
    }

    mOFN.lpstrFile = mFilesW;
    if (!filename.empty())
    {
        std::wstring tstring = ll_convert<std::wstring>(filename);
        wcsncpy(mFilesW, tstring.c_str(), FILENAME_BUFFER_SIZE);    }   /*Flawfinder: ignore*/
    else
    {
        mFilesW[0] = '\0';
    }
    mOFN.hwndOwner = (HWND)gViewerWindow->getPlatformWindow();

    switch( filter )
    {
    case FFSAVE_ALL:
        mOFN.lpstrDefExt = NULL;
        mOFN.lpstrFilter =
            L"All Files (*.*)\0*.*\0" \
            L"WAV Sounds (*.wav)\0*.wav\0" \
            L"Targa, Bitmap Images (*.tga; *.bmp)\0*.tga;*.bmp\0" \
            L"\0";
        break;
    case FFSAVE_WAV:
        if (filename.empty())
        {
            wcsncpy( mFilesW,L"untitled.wav", FILENAME_BUFFER_SIZE);    /*Flawfinder: ignore*/
        }
        mOFN.lpstrDefExt = L"wav";
        mOFN.lpstrFilter =
            L"WAV Sounds (*.wav)\0*.wav\0" \
            L"\0";
        break;
    case FFSAVE_TGA:
        if (filename.empty())
        {
            wcsncpy( mFilesW,L"untitled.tga", FILENAME_BUFFER_SIZE);    /*Flawfinder: ignore*/
        }
        mOFN.lpstrDefExt = L"tga";
        mOFN.lpstrFilter =
            L"Targa Images (*.tga)\0*.tga\0" \
            L"\0";
        break;
    case FFSAVE_BMP:
        if (filename.empty())
        {
            wcsncpy( mFilesW,L"untitled.bmp", FILENAME_BUFFER_SIZE);    /*Flawfinder: ignore*/
        }
        mOFN.lpstrDefExt = L"bmp";
        mOFN.lpstrFilter =
            L"Bitmap Images (*.bmp)\0*.bmp\0" \
            L"\0";
        break;
    case FFSAVE_PNG:
        if (filename.empty())
        {
            wcsncpy( mFilesW,L"untitled.png", FILENAME_BUFFER_SIZE);    /*Flawfinder: ignore*/
        }
        mOFN.lpstrDefExt = L"png";
        mOFN.lpstrFilter =
            L"PNG Images (*.png)\0*.png\0" \
            L"\0";
        break;
    case FFSAVE_TGAPNG:
        if (filename.empty())
        {
            wcsncpy( mFilesW,L"untitled.png", FILENAME_BUFFER_SIZE);    /*Flawfinder: ignore*/
            //PNG by default
        }
        mOFN.lpstrDefExt = L"png";
        mOFN.lpstrFilter =
            L"PNG Images (*.png)\0*.png\0" \
            L"Targa Images (*.tga)\0*.tga\0" \
            L"\0";
        break;

    case FFSAVE_JPEG:
        if (filename.empty())
        {
            wcsncpy( mFilesW,L"untitled.jpeg", FILENAME_BUFFER_SIZE);   /*Flawfinder: ignore*/
        }
        mOFN.lpstrDefExt = L"jpg";
        mOFN.lpstrFilter =
            L"JPEG Images (*.jpg *.jpeg)\0*.jpg;*.jpeg\0" \
            L"\0";
        break;
    case FFSAVE_AVI:
        if (filename.empty())
        {
            wcsncpy( mFilesW,L"untitled.avi", FILENAME_BUFFER_SIZE);    /*Flawfinder: ignore*/
        }
        mOFN.lpstrDefExt = L"avi";
        mOFN.lpstrFilter =
            L"AVI Movie File (*.avi)\0*.avi\0" \
            L"\0";
        break;
    case FFSAVE_ANIM:
        if (filename.empty())
        {
            wcsncpy( mFilesW,L"untitled.xaf", FILENAME_BUFFER_SIZE);    /*Flawfinder: ignore*/
        }
        mOFN.lpstrDefExt = L"xaf";
        mOFN.lpstrFilter =
            L"XAF Anim File (*.xaf)\0*.xaf\0" \
            L"\0";
        break;
    case FFSAVE_GLTF:
        if (filename.empty())
        {
            wcsncpy( mFilesW,L"untitled.gltf", FILENAME_BUFFER_SIZE);   /*Flawfinder: ignore*/
        }
        mOFN.lpstrDefExt = L"gltf";
        mOFN.lpstrFilter =
            L"glTF Asset File (*.gltf)\0*.gltf\0" \
            L"\0";
        break;
    case FFSAVE_XML:
        if (filename.empty())
        {
            wcsncpy( mFilesW,L"untitled.xml", FILENAME_BUFFER_SIZE);    /*Flawfinder: ignore*/
        }

        mOFN.lpstrDefExt = L"xml";
        mOFN.lpstrFilter =
            L"XML File (*.xml)\0*.xml\0" \
            L"\0";
        break;
    case FFSAVE_COLLADA:
        if (filename.empty())
        {
            wcsncpy( mFilesW,L"untitled.dae", FILENAME_BUFFER_SIZE);    /*Flawfinder: ignore*/
        }
        mOFN.lpstrDefExt = L"dae";
        mOFN.lpstrFilter =
            L"COLLADA File (*.dae)\0*.dae\0" \
            L"\0";
        break;
    case FFSAVE_RAW:
        if (filename.empty())
        {
            wcsncpy( mFilesW,L"untitled.raw", FILENAME_BUFFER_SIZE);    /*Flawfinder: ignore*/
        }
        mOFN.lpstrDefExt = L"raw";
        mOFN.lpstrFilter =  RAW_FILTER \
                            L"\0";
        break;
    case FFSAVE_J2C:
        if (filename.empty())
        {
            wcsncpy( mFilesW,L"untitled.j2c", FILENAME_BUFFER_SIZE);
        }
        mOFN.lpstrDefExt = L"j2c";
        mOFN.lpstrFilter =
            L"Compressed Images (*.j2c)\0*.j2c\0" \
            L"\0";
        break;
    case FFSAVE_SCRIPT:
        if (filename.empty())
        {
            wcsncpy( mFilesW,L"untitled.lsl", FILENAME_BUFFER_SIZE);
        }
        mOFN.lpstrDefExt = L"txt";
        mOFN.lpstrFilter = L"LSL Files (*.lsl)\0*.lsl\0" L"\0";
        break;

// <Firestorm>
    case FFSAVE_BEAM:
        if (filename.empty())
        {
            wcsncpy( mFilesW,L"untitled.xml", FILENAME_BUFFER_SIZE);    /*Flawfinder: ignore*/
        }
        mOFN.lpstrDefExt = L"xml";
        mOFN.lpstrFilter =
            L"XML File (*.xml)\0*.xml\0" \
            L"\0";
        break;
    case FFSAVE_EXPORT:
        if (filename.empty())
        {
            wcsncpy( mFilesW,L"untitled.oxp", FILENAME_BUFFER_SIZE);
        }
        mOFN.lpstrDefExt = L"oxp";
        mOFN.lpstrFilter = L"OXP Backup Files (*.oxp)\0*.oxp\0" L"\0";
        break;
    case FFSAVE_CSV:
        if (filename.empty())
        {
            wcsncpy( mFilesW, L"untitled.csv", FILENAME_BUFFER_SIZE);
        }
        mOFN.lpstrDefExt = L".csv";
        mOFN.lpstrFilter =
        L"Comma seperated values (*.csv)\0*.csv\0" \
        L"\0";
        break;
// </Firestorm>
    default:
        return false;
    }


    mOFN.nMaxFile = SINGLE_FILENAME_BUFFER_SIZE;
    mOFN.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST;

    reset();

    if (blocking)
    {
        // Modal, so pause agent
        send_agent_pause();
    }

    {
        // NOTA BENE: hitting the file dialog triggers a window focus event, destroying the selection manager!!
        try
        {
            success = GetSaveFileName(&mOFN);
            if (success)
            {
                std::string filename = ll_convert<std::string>(std::wstring(mFilesW));
                mFiles.push_back(filename);
            }
        }
        catch (...)
        {
            LOG_UNHANDLED_EXCEPTION("");
        }
        gKeyboard->resetKeys();
    }

    if (blocking)
    {
        send_agent_resume();
    }

    // Account for the fact that the app has been stalled.
    LLFrameTimer::updateFrameTime();
    return success;
}

bool LLFilePicker::getSaveFileModeless(ESaveFilter filter,
                                       const std::string& filename,
                                       void (*callback)(bool, std::string&, void*),
                                       void *userdata)
{
    // not supposed to be used yet, use LLFilePickerThread
    LL_ERRS() << "NOT IMPLEMENTED" << LL_ENDL;
    return false;
}

#elif LL_DARWIN

std::unique_ptr<std::vector<std::string>> LLFilePicker::navOpenFilterProc(ELoadFilter filter) //(AEDesc *theItem, void *info, void *callBackUD, NavFilterModes filterMode)
{
    std::unique_ptr<std::vector<std::string>> allowedv(new std::vector< std::string >);
    switch(filter)
    {
        case FFLOAD_ALL:
        case FFLOAD_EXE:
            allowedv->push_back("app");
            allowedv->push_back("exe");
            allowedv->push_back("wav");
            allowedv->push_back("bvh");
            allowedv->push_back("anim");
            allowedv->push_back("dae");
            allowedv->push_back("raw");
            allowedv->push_back("lsl");
            allowedv->push_back("dic");
            allowedv->push_back("xcu");
            allowedv->push_back("gif");
            allowedv->push_back("gltf");
            allowedv->push_back("glb");
            allowedv->push_back("xml");
            // <FS:CR> Import filter
            allowedv->push_back("oxp");
            //allowedv->push_back("hpa");
            // </FS:CR>
        case FFLOAD_IMAGE:
            allowedv->push_back("jpg");
            allowedv->push_back("jpeg");
            allowedv->push_back("bmp");
            allowedv->push_back("tga");
            allowedv->push_back("bmpf");
            allowedv->push_back("tpic");
            allowedv->push_back("png");
            break;
            break;
        case FFLOAD_WAV:
            allowedv->push_back("wav");
            break;
        case FFLOAD_ANIM:
            allowedv->push_back("bvh");
            allowedv->push_back("anim");
            break;
        case FFLOAD_GLTF:
        case FFLOAD_MATERIAL:
            allowedv->push_back("gltf");
            allowedv->push_back("glb");
            break;
        case FFLOAD_HDRI:
            allowedv->push_back("exr");
            break;
        case FFLOAD_MODEL:
            allowedv->push_back("gltf");
            allowedv->push_back("glb");
        case FFLOAD_COLLADA:
            allowedv->push_back("dae");
            break;
        case FFLOAD_XML:
            allowedv->push_back("xml");
            break;
        case FFLOAD_RAW:
            allowedv->push_back("raw");
            break;
        case FFLOAD_SCRIPT:
            allowedv->push_back("lsl");
            break;
        case FFLOAD_DICTIONARY:
            allowedv->push_back("dic");
            allowedv->push_back("xcu");
            break;
        case FFLOAD_DIRECTORY:
            break;
        // <FS:CR> Import filter
        case FFLOAD_IMPORT:
            allowedv->push_back("oxp");
            //allowedv->push_back("hpa");
            break;
        // </FS:CR>
        default:
            LL_WARNS() << "Unsupported format." << LL_ENDL;
    }

    return allowedv;
}

bool LLFilePicker::doNavChooseDialog(ELoadFilter filter)
{
    // if local file browsing is turned off, return without opening dialog
    if (!check_local_file_access_enabled())
    {
        return false;
    }

    gViewerWindow->getWindow()->beforeDialog();

    std::unique_ptr<std::vector<std::string>> allowed_types = navOpenFilterProc(filter);

    std::unique_ptr<std::vector<std::string>> filev  = doLoadDialog(allowed_types.get(),
                                                    mPickOptions);

    gViewerWindow->getWindow()->afterDialog();


    if (filev && filev->size() > 0)
    {
        mFiles.insert(mFiles.end(), filev->begin(), filev->end());
        return true;
    }

    return false;
}

bool LLFilePicker::doNavChooseDialogModeless(ELoadFilter filter,
                                                void (*callback)(bool, std::vector<std::string> &,void*),
                                                void *userdata)
{
    // if local file browsing is turned off, return without opening dialog
    if (!check_local_file_access_enabled())
    {
        return false;
    }

    std::unique_ptr<std::vector<std::string>> allowed_types=navOpenFilterProc(filter);

    doLoadDialogModeless(allowed_types.get(),
                                                    mPickOptions,
                                                    callback,
                                                    userdata);

    return true;
}

void set_nav_save_data(LLFilePicker::ESaveFilter filter, std::string &extension, std::string &type, std::string &creator)
{
    switch (filter)
    {
        case LLFilePicker::FFSAVE_WAV:
            type = "WAVE";
            creator = "TVOD";
            extension = "wav";
            break;
        case LLFilePicker::FFSAVE_TGA:
            type = "TPIC";
            creator = "prvw";
            extension = "tga";
            break;
        case LLFilePicker::FFSAVE_TGAPNG:
            type = "PNG";
            creator = "prvw";
            extension = "png,tga";
            break;
        case LLFilePicker::FFSAVE_BMP:
            type = "BMPf";
            creator = "prvw";
            extension = "bmp";
            break;
        case LLFilePicker::FFSAVE_JPEG:
            type = "JPEG";
            creator = "prvw";
            extension = "jpeg";
            break;
        case LLFilePicker::FFSAVE_PNG:
            type = "PNG ";
            creator = "prvw";
            extension = "png";
            break;
        case LLFilePicker::FFSAVE_AVI:
            type = "\?\?\?\?";
            creator = "\?\?\?\?";
            extension = "mov";
            break;

        case LLFilePicker::FFSAVE_ANIM:
            type = "\?\?\?\?";
            creator = "\?\?\?\?";
            extension = "xaf";
            break;
        case LLFilePicker::FFSAVE_GLTF:
            type = "\?\?\?\?";
            creator = "\?\?\?\?";
            extension = "gltf";
            break;

        // <FS:TS> Compile fix
        //case LLFilePicker::FFSAVE_XML:
        //    type = "\?\?\?\?";
        //    creator = "\?\?\?\?";
        //    extension = "xml";
        //    break;
        // </FS:TS> Compile fix

        case LLFilePicker::FFSAVE_RAW:
            type = "\?\?\?\?";
            creator = "\?\?\?\?";
            extension = "raw";
            break;

        case LLFilePicker::FFSAVE_J2C:
            type = "\?\?\?\?";
            creator = "prvw";
            extension = "j2c";
            break;

        case LLFilePicker::FFSAVE_SCRIPT:
            type = "LSL ";
            creator = "\?\?\?\?";
            extension = "lsl";
            break;

        // <FS:CR> Export filter
        case LLFilePicker::FFSAVE_EXPORT:
            type = "OXP ";
            creator = "\?\?\?\?";
            extension = "oxp";
            break;
        case LLFilePicker::FFSAVE_COLLADA:
            type = "DAE ";
            creator = "\?\?\?\?";
            extension = "dae";
            break;
        // <FS:CR> CSV Filter
        case LLFilePicker::FFSAVE_CSV:
            type = "CSV ";
            creator = "\?\?\?\?";
            extension = "csv";
            break;
        // </FS:CR>
        case LLFilePicker::FFSAVE_BEAM:
        case LLFilePicker::FFSAVE_XML:
            type = "XML ";
            creator = "\?\?\?\?";
            extension = "xml";
            break;

        case LLFilePicker::FFSAVE_ALL:
        default:
            type = "\?\?\?\?";
            creator = "\?\?\?\?";
            extension = "";
            break;
    }
}

bool LLFilePicker::doNavSaveDialog(ESaveFilter filter, const std::string& filename)
{
    // Setup the type, creator, and extension
    std::string     extension, type, creator;

    set_nav_save_data(filter, extension, type, creator);

    std::string namestring = filename;
    if (namestring.empty()) namestring="Untitled";

    gViewerWindow->getWindow()->beforeDialog();

    // Run the dialog
    std::unique_ptr<std::string> filev = doSaveDialog(&namestring,
                 &type,
                 &creator,
                 &extension,
                 mPickOptions);

    gViewerWindow->getWindow()->afterDialog();

    if ( filev && !filev->empty() )
    {
        mFiles.push_back(*filev);
        return true;
    }

    return false;
}

bool LLFilePicker::doNavSaveDialogModeless(ESaveFilter filter,
                                              const std::string& filename,
                                              void (*callback)(bool, std::string&, void*),
                                              void *userdata)
{
    // Setup the type, creator, and extension
    std::string        extension, type, creator;

    set_nav_save_data(filter, extension, type, creator);

    std::string namestring = filename;
    if (namestring.empty()) namestring="Untitled";

    // Run the dialog
    doSaveDialogModeless(&namestring,
                 &type,
                 &creator,
                 &extension,
                 mPickOptions,
                 callback,
                 userdata);
    return true;
}

bool LLFilePicker::getOpenFile(ELoadFilter filter, bool blocking)
{
    if( mLocked )
        return false;

    bool success = false;

    // if local file browsing is turned off, return without opening dialog
    if (!check_local_file_access_enabled())
    {
        return false;
    }

    reset();

    mPickOptions &= ~F_MULTIPLE;
    mPickOptions |= F_FILE;

    if (filter == FFLOAD_DIRECTORY) //This should only be called from lldirpicker.
    {
        mPickOptions |= ( F_NAV_SUPPORT | F_DIRECTORY );
        mPickOptions &= ~F_FILE;
    }

    if (filter == FFLOAD_ALL)   // allow application bundles etc. to be traversed; important for DEV-16869, but generally useful
    {
        mPickOptions |= F_NAV_SUPPORT;
    }

    if (blocking) // always true for linux/mac
    {
        // Modal, so pause agent
        send_agent_pause();
    }


    success = doNavChooseDialog(filter);

    if (success)
    {
        if (!getFileCount())
            success = false;
    }

    if (blocking)
    {
        send_agent_resume();
        // Account for the fact that the app has been stalled.
        LLFrameTimer::updateFrameTime();
    }

    return success;
}


bool LLFilePicker::getOpenFileModeless(ELoadFilter filter,
                                       void (*callback)(bool, std::vector<std::string> &, void*),
                                       void *userdata)
{
    if (mLocked)
        return false;

    // if local file browsing is turned off, return without opening dialog
    if (!check_local_file_access_enabled())
    {
        return false;
    }

    reset();

    mPickOptions &= ~F_MULTIPLE;
    mPickOptions |= F_FILE;

    if (filter == FFLOAD_DIRECTORY) //This should only be called from lldirpicker.
    {

        mPickOptions |= ( F_NAV_SUPPORT | F_DIRECTORY );
        mPickOptions &= ~F_FILE;
    }

    if (filter == FFLOAD_ALL)    // allow application bundles etc. to be traversed; important for DEV-16869, but generally useful
    {
        mPickOptions |= F_NAV_SUPPORT;
    }

    return doNavChooseDialogModeless(filter, callback, userdata);
}

bool LLFilePicker::getMultipleOpenFiles(ELoadFilter filter, bool blocking)
{
    if (mLocked)
        return false;

    // if local file browsing is turned off, return without opening dialog
    if (!check_local_file_access_enabled())
    {
        return false;
    }

    bool success = false;

    reset();

    mPickOptions |= F_FILE;

    mPickOptions |= F_MULTIPLE;

    if (blocking) // always true for linux/mac
    {
        // Modal, so pause agent
        send_agent_pause();
    }

    success = doNavChooseDialog(filter);

    if (blocking)
    {
        send_agent_resume();
    }

    if (success)
    {
        if (!getFileCount())
            success = false;
        if (getFileCount() > 1)
            mLocked = true;
    }

    // Account for the fact that the app has been stalled.
    LLFrameTimer::updateFrameTime();
    return success;
}


bool LLFilePicker::getMultipleOpenFilesModeless(ELoadFilter filter,
                                                void (*callback)(bool, std::vector<std::string> &, void*),
                                                void *userdata )
{
    if (mLocked)
        return false;

    // if local file browsing is turned off, return without opening dialog
    if (!check_local_file_access_enabled())
    {
        return false;
    }

    reset();

    mPickOptions |= F_FILE;

    mPickOptions |= F_MULTIPLE;

    return doNavChooseDialogModeless(filter, callback, userdata);
}

bool LLFilePicker::getSaveFile(ESaveFilter filter, const std::string& filename, bool blocking)
{

    if (mLocked)
        return false;

    bool success = false;

    // if local file browsing is turned off, return without opening dialog
    if (!check_local_file_access_enabled())
    {
        return false;
    }

    reset();

    mPickOptions &= ~F_MULTIPLE;

    if (blocking)
    {
        // Modal, so pause agent
        send_agent_pause();
    }

    success = doNavSaveDialog(filter, filename);

    if (success)
    {
        if (!getFileCount())
            success = false;
    }

    if (blocking)
    {
        send_agent_resume();
    }

    // Account for the fact that the app has been stalled.
    LLFrameTimer::updateFrameTime();
    return success;
}

bool LLFilePicker::getSaveFileModeless(ESaveFilter filter,
                                       const std::string& filename,
                                       void (*callback)(bool, std::string&, void*),
                                       void *userdata)
{
    if (mLocked)
        return false;

    // if local file browsing is turned off, return without opening dialog
    if (!check_local_file_access_enabled())
    {
        return false;
    }

    reset();

    mPickOptions &= ~F_MULTIPLE;

    return doNavSaveDialogModeless(filter, filename, callback, userdata);
}
//END LL_DARWIN

#elif LL_LINUX

# if LL_GTK

// static
void LLFilePicker::add_to_selectedfiles(gpointer data, gpointer user_data)
{
    // We need to run g_filename_to_utf8 in the user's locale
    std::string saved_locale(setlocale(LC_ALL, NULL));
    setlocale(LC_ALL, "");

    LLFilePicker* picker = (LLFilePicker*) user_data;
    GError *error = NULL;
    gchar* filename_utf8 = g_filename_to_utf8((gchar*)data,
                          -1, NULL, NULL, &error);
    if (error)
    {
        // *FIXME.
        // This condition should really be notified to the user, e.g.
        // through a message box.  Just logging it is inappropriate.

        // g_filename_display_name is ideal, but >= glib 2.6, so:
        // a hand-rolled hacky makeASCII which disallows control chars
        std::string display_name;
        for (const gchar *str = (const gchar *)data; *str; str++)
        {
            display_name += (char)((*str >= 0x20 && *str <= 0x7E) ? *str : '?');
        }
        LL_WARNS() << "g_filename_to_utf8 failed on \"" << display_name << "\": " << error->message << LL_ENDL;
    }

    if (filename_utf8)
    {
        picker->mFiles.push_back(std::string(filename_utf8));
        LL_DEBUGS() << "ADDED FILE " << filename_utf8 << LL_ENDL;
        g_free(filename_utf8);
    }

    setlocale(LC_ALL, saved_locale.c_str());
}

// static
void LLFilePicker::chooser_responder(GtkWidget *widget, gint response, gpointer user_data)
{
    LLFilePicker* picker = (LLFilePicker*)user_data;

    LL_DEBUGS() << "GTK DIALOG RESPONSE " << response << LL_ENDL;

    if (response == GTK_RESPONSE_ACCEPT)
    {
        GSList *file_list = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(widget));
        g_slist_foreach(file_list, (GFunc)add_to_selectedfiles, user_data);
        g_slist_foreach(file_list, (GFunc)g_free, NULL);
        g_slist_free (file_list);
    }

    // let's save the extension of the last added file(considering current filter)
    GtkFileFilter *gfilter = gtk_file_chooser_get_filter(GTK_FILE_CHOOSER(widget));
    if(gfilter)
    {
        std::string filter = gtk_file_filter_get_name(gfilter);

        if(filter == LLTrans::getString("png_image_files"))
        {
            picker->mCurrentExtension = ".png";
        }
        else if(filter == LLTrans::getString("targa_image_files"))
        {
            picker->mCurrentExtension = ".tga";
        }
    }

    // set the default path for this usage context.
    const char* cur_folder = gtk_file_chooser_get_current_folder(GTK_FILE_CHOOSER(widget));
    if (cur_folder != NULL)
    {
        // <FS> FIRE-14924: Remember last used directory
        //picker->mContextToPathMap[picker->mCurContextName] = cur_folder;
        if (picker->mCurContextName == "openfile")
        {
            gSavedSettings.setString("FSFilePickerOpenDirectory", cur_folder);
        }
        else if (picker->mCurContextName == "savefile")
        {
            gSavedSettings.setString("FSFilePickerSaveDirectory", cur_folder);
        }
        // </FS>
    }

    gtk_widget_destroy(widget);
    gtk_main_quit();
}


GtkWindow* LLFilePicker::buildFilePicker(bool is_save, bool is_folder, std::string context)
{
#ifndef LL_MESA_HEADLESS
    if (LLWindowSDL::ll_try_gtk_init())
    {
        GtkWidget *win = NULL;
        GtkFileChooserAction pickertype =
            is_save?
            (is_folder?
             GTK_FILE_CHOOSER_ACTION_CREATE_FOLDER :
             GTK_FILE_CHOOSER_ACTION_SAVE) :
            (is_folder?
             GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER :
             GTK_FILE_CHOOSER_ACTION_OPEN);

        win = gtk_file_chooser_dialog_new(NULL, NULL,
                          pickertype,
                          GTK_STOCK_CANCEL,
                           GTK_RESPONSE_CANCEL,
                          is_folder ?
                          GTK_STOCK_APPLY :
                          (is_save ?
                           GTK_STOCK_SAVE :
                           GTK_STOCK_OPEN),
                           GTK_RESPONSE_ACCEPT,
                          (gchar *)NULL);
        mCurContextName = context;

        // get the default path for this usage context if it's been
        // seen before.
        // <FS> FIRE-14924: Remember last used directory
        //std::map<std::string,std::string>::iterator
        //  this_path = mContextToPathMap.find(context);
        //if (this_path != mContextToPathMap.end())
        //{
        //  gtk_file_chooser_set_current_folder
        //      (GTK_FILE_CHOOSER(win),
        //       this_path->second.c_str());
        //}
        std::string this_path = "";

        if (context == "openfile")
        {
            this_path = gSavedSettings.getString("FSFilePickerOpenDirectory");
        }
        else if (context == "savefile")
        {
            this_path = gSavedSettings.getString("FSFilePickerSaveDirectory");
        }

        if (!this_path.empty())
        {
            gtk_file_chooser_set_current_folder
                (GTK_FILE_CHOOSER(win),
                 this_path.c_str());
        }
        // </FS>

#  if LL_X11
        // Make GTK tell the window manager to associate this
        // dialog with our non-GTK raw X11 window, which should try
        // to keep it on top etc.
        Window XWindowID = LLWindowSDL::get_SDL_XWindowID();
        if (None != XWindowID)
        {
            gtk_widget_realize(GTK_WIDGET(win)); // so we can get its gdkwin
            GdkWindow *gdkwin = gdk_window_foreign_new(XWindowID);
            gdk_window_set_transient_for(GTK_WIDGET(win)->window,
                             gdkwin);
        }
        else
        {
            LL_WARNS() << "Hmm, couldn't get xwid to use for transient." << LL_ENDL;
        }
#  endif //LL_X11

        g_signal_connect (GTK_FILE_CHOOSER(win),
                  "response",
                  G_CALLBACK(LLFilePicker::chooser_responder),
                  this);

        gtk_window_set_modal(GTK_WINDOW(win), TRUE);

        /* GTK 2.6: if (is_folder)
            gtk_file_chooser_set_show_hidden(GTK_FILE_CHOOSER(win),
            TRUE); */

        return GTK_WINDOW(win);
    }
    else
    {
        return NULL;
    }
#else
    return NULL;
#endif //LL_MESA_HEADLESS
}

static void add_common_filters_to_gtkchooser(GtkFileFilter *gfilter,
                         GtkWindow *picker,
                         std::string filtername)
{
    gtk_file_filter_set_name(gfilter, filtername.c_str());
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(picker),
                    gfilter);
    GtkFileFilter *allfilter = gtk_file_filter_new();
    gtk_file_filter_add_pattern(allfilter, "*");
    gtk_file_filter_set_name(allfilter, LLTrans::getString("all_files").c_str());
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(picker), allfilter);
    gtk_file_chooser_set_filter(GTK_FILE_CHOOSER(picker), gfilter);
}

static std::string add_simple_pattern_filter_to_gtkchooser(GtkWindow *picker,
                               std::string pattern,
                               std::string filtername)
{
    GtkFileFilter *gfilter = gtk_file_filter_new();
    gtk_file_filter_add_pattern(gfilter, pattern.c_str());
    add_common_filters_to_gtkchooser(gfilter, picker, filtername);
    return filtername;
}

static std::string add_simple_mime_filter_to_gtkchooser(GtkWindow *picker,
                            std::string mime,
                            std::string filtername)
{
    GtkFileFilter *gfilter = gtk_file_filter_new();
    gtk_file_filter_add_mime_type(gfilter, mime.c_str());
    add_common_filters_to_gtkchooser(gfilter, picker, filtername);
    return filtername;
}

static std::string add_wav_filter_to_gtkchooser(GtkWindow *picker)
{
    return add_simple_mime_filter_to_gtkchooser(picker,  "audio/x-wav",
                            LLTrans::getString("sound_files") + " (*.wav)");
}

static std::string add_anim_filter_to_gtkchooser(GtkWindow *picker)
{
    GtkFileFilter *gfilter = gtk_file_filter_new();
    gtk_file_filter_add_pattern(gfilter, "*.bvh");
    gtk_file_filter_add_pattern(gfilter, "*.anim");
    std::string filtername = LLTrans::getString("animation_files") + " (*.bvh; *.anim)";
    add_common_filters_to_gtkchooser(gfilter, picker, filtername);
    return filtername;
}

static std::string add_xml_filter_to_gtkchooser(GtkWindow *picker)
{
    return add_simple_pattern_filter_to_gtkchooser(picker,  "*.xml",
                                                   LLTrans::getString("xml_file") + " (*.xml)");
}

static std::string add_collada_filter_to_gtkchooser(GtkWindow *picker)
{
    return add_simple_pattern_filter_to_gtkchooser(picker,  "*.dae",
                               LLTrans::getString("collada_files") + " (*.dae)");
}

// <FS:Beq> migrate to GLTF support
static std::string add_model_filter_to_gtkchooser(GtkWindow *picker)
{
// "Model files (*.dae, *.gltf, *.glb)"
    GtkFileFilter *gfilter = gtk_file_filter_new();
    gtk_file_filter_add_pattern(gfilter, "*.dae");
    gtk_file_filter_add_pattern(gfilter, "*.gltf");
    gtk_file_filter_add_pattern(gfilter, "*.glb");
    std::string filtername = LLTrans::getString("model_files") + " (*.dae; *.gltf; *.glb)";
    add_common_filters_to_gtkchooser(gfilter, picker, filtername);
    return filtername;
}
// </FS:Beq>

static std::string add_imageload_filter_to_gtkchooser(GtkWindow *picker)
{
    GtkFileFilter *gfilter = gtk_file_filter_new();
    gtk_file_filter_add_pattern(gfilter, "*.tga");
    gtk_file_filter_add_mime_type(gfilter, HTTP_CONTENT_IMAGE_JPEG.c_str());
    gtk_file_filter_add_mime_type(gfilter, HTTP_CONTENT_IMAGE_PNG.c_str());
    gtk_file_filter_add_mime_type(gfilter, HTTP_CONTENT_IMAGE_BMP.c_str());
    std::string filtername = LLTrans::getString("image_files") + " (*.tga; *.bmp; *.jpg; *.png)";
    add_common_filters_to_gtkchooser(gfilter, picker, filtername);
    return filtername;
}

static std::string add_script_filter_to_gtkchooser(GtkWindow *picker)
{
    return add_simple_mime_filter_to_gtkchooser(picker,  HTTP_CONTENT_TEXT_PLAIN,
                            LLTrans::getString("script_files") + " (*.lsl)");
}

static std::string add_dictionary_filter_to_gtkchooser(GtkWindow *picker)
{
    return add_simple_mime_filter_to_gtkchooser(picker, HTTP_CONTENT_TEXT_PLAIN,
                            LLTrans::getString("dictionary_files") + " (*.dic; *.xcu)");
}

// <FS:CR> GTK Import/Export filters
static std::string add_import_filter_to_gtkchooser(GtkWindow *picker)
{
    GtkFileFilter *gfilter = gtk_file_filter_new();
    gtk_file_filter_add_pattern(gfilter, "*.oxp");
    std::string filtername = LLTrans::getString("backup_files") + " (*.oxp)";
    //gtk_file_filter_add_pattern(gfilter, "*.hpa");
    //std::string filtername = LLTrans::getString("backup_files") + " (*.oxp; *.hpa)";
    add_common_filters_to_gtkchooser(gfilter, picker, filtername);
    return filtername;
}
// </FS:CR>

static std::string add_save_texture_filter_to_gtkchooser(GtkWindow *picker)
{
    GtkFileFilter *gfilter_tga = gtk_file_filter_new();
    GtkFileFilter *gfilter_png = gtk_file_filter_new();

    gtk_file_filter_add_pattern(gfilter_tga, "*.tga");
    gtk_file_filter_add_mime_type(gfilter_png, "image/png");
    std::string caption = LLTrans::getString("save_texture_image_files") + " (*.tga; *.png)";
    gtk_file_filter_set_name(gfilter_tga, LLTrans::getString("targa_image_files").c_str());
    gtk_file_filter_set_name(gfilter_png, LLTrans::getString("png_image_files").c_str());

    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(picker),
                    gfilter_png);
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(picker),
                    gfilter_tga);
    return caption;
}

bool LLFilePicker::getSaveFile( ESaveFilter filter, const std::string& filename, bool blocking )
{
    bool rtn = false;

    // if local file browsing is turned off, return without opening dialog
    if (!check_local_file_access_enabled())
    {
        return false;
    }

    gViewerWindow->getWindow()->beforeDialog();

    reset();

    GtkWindow* picker = buildFilePicker(true, false, "savefile");

    if (picker)
    {
        std::string suggest_name = "untitled";
        std::string suggest_ext = "";
        std::string caption = LLTrans::getString("save_file_verb") + " ";
        switch (filter)
        {
        case FFSAVE_WAV:
            caption += add_wav_filter_to_gtkchooser(picker);
            suggest_ext = ".wav";
            break;
        case FFSAVE_TGA:
            caption += add_simple_pattern_filter_to_gtkchooser
                (picker, "*.tga", LLTrans::getString("targa_image_files") + " (*.tga)");
            suggest_ext = ".tga";
            break;
        case FFSAVE_BMP:
            caption += add_simple_mime_filter_to_gtkchooser
                (picker, HTTP_CONTENT_IMAGE_BMP, LLTrans::getString("bitmap_image_files") + " (*.bmp)");
            suggest_ext = ".bmp";
            break;
        case FFSAVE_PNG:
            caption += add_simple_mime_filter_to_gtkchooser
                (picker, "image/png", LLTrans::getString("png_image_files") + " (*.png)");
            suggest_ext = ".png";
            break;
        case FFSAVE_TGAPNG:
            caption += add_save_texture_filter_to_gtkchooser(picker);
            suggest_ext = ".png";
            break;
        case FFSAVE_AVI:
            caption += add_simple_mime_filter_to_gtkchooser
                (picker, "video/x-msvideo",
                 LLTrans::getString("avi_movie_file") + " (*.avi)");
            suggest_ext = ".avi";
            break;
        case FFSAVE_ANIM:
            caption += add_simple_pattern_filter_to_gtkchooser
                (picker, "*.xaf", LLTrans::getString("xaf_animation_file") + " (*.xaf)");
            suggest_ext = ".xaf";
            break;
        case FFSAVE_XML:
            caption += add_simple_pattern_filter_to_gtkchooser
                (picker, "*.xml", LLTrans::getString("xml_file") + " (*.xml)");
            suggest_ext = ".xml";
            break;
        case FFSAVE_RAW:
            caption += add_simple_pattern_filter_to_gtkchooser
                (picker, "*.raw", LLTrans::getString("raw_file") + " (*.raw)");
            suggest_ext = ".raw";
            break;
        case FFSAVE_J2C:
            // *TODO: Should this be 'image/j2c' ?
            caption += add_simple_mime_filter_to_gtkchooser
                (picker, "images/jp2",
                 LLTrans::getString("compressed_image_files") + " (*.j2c)");
            suggest_ext = ".j2c";
            break;
        case FFSAVE_SCRIPT:
            caption += add_script_filter_to_gtkchooser(picker);
            suggest_ext = ".lsl";
            break;
// <FS:CR> Export filter
        case FFSAVE_EXPORT:
            caption += add_simple_pattern_filter_to_gtkchooser
                (picker, "*.oxp", LLTrans::getString("backup_files") + " (*.oxp)");
            break;
        case FFSAVE_COLLADA:
            caption += add_simple_pattern_filter_to_gtkchooser
                (picker, "*.dae", LLTrans::getString("collada_files") + " (*.dae)");
            break;
        // [FS:CR] FIRE-12276
        case FFSAVE_CSV:
            caption += add_simple_pattern_filter_to_gtkchooser
                (picker, "*.csv", LLTrans::getString("csv_files") + " (*.csv)");
// </FS:CR>
        default:;
            break;
        }

        gtk_window_set_title(GTK_WINDOW(picker), caption.c_str());

        if (filename.empty())
        {
            suggest_name += suggest_ext;

            gtk_file_chooser_set_current_name
                (GTK_FILE_CHOOSER(picker),
                 suggest_name.c_str());
        }
        else
        {
            gtk_file_chooser_set_current_name
                (GTK_FILE_CHOOSER(picker), filename.c_str());
        }

        gtk_widget_show_all(GTK_WIDGET(picker));

        gtk_main();

        rtn = (getFileCount() == 1);

        if(rtn && filter == FFSAVE_TGAPNG)
        {
            std::string selected_file = mFiles.back();
            mFiles.pop_back();
            mFiles.push_back(selected_file + mCurrentExtension);
        }
    }

    gViewerWindow->getWindow()->afterDialog();

    return rtn;
}

bool LLFilePicker::getOpenFile( ELoadFilter filter, bool blocking )
{
    bool rtn = false;

    // if local file browsing is turned off, return without opening dialog
    if (!check_local_file_access_enabled())
    {
        return false;
    }

    gViewerWindow->getWindow()->beforeDialog();

    reset();

    GtkWindow* picker = buildFilePicker(false, false, "openfile");

    if (picker)
    {
        std::string caption = LLTrans::getString("load_file_verb") + " ";
        std::string filtername = "";
        switch (filter)
        {
        case FFLOAD_WAV:
            filtername = add_wav_filter_to_gtkchooser(picker);
            break;
        case FFLOAD_ANIM:
            filtername = add_anim_filter_to_gtkchooser(picker);
            break;
        case FFLOAD_XML:
            filtername = add_xml_filter_to_gtkchooser(picker);
            break;
        case FFLOAD_GLTF:
            filtername = dead_code_should_blow_up_here(picker);
            break;
        case FFLOAD_COLLADA:
            filtername = add_model_filter_to_gtkchooser(picker);
            break;
        case FFLOAD_IMAGE:
            filtername = add_imageload_filter_to_gtkchooser(picker);
            break;
        case FFLOAD_SCRIPT:
            filtername = add_script_filter_to_gtkchooser(picker);
            break;
        case FFLOAD_DICTIONARY:
            filtername = add_dictionary_filter_to_gtkchooser(picker);
            break;
// <FS:CR> Import filter
        case FFLOAD_IMPORT:
            filtername = add_import_filter_to_gtkchooser(picker);
            break;
// </FS:CR>
        default:;
            break;
        }

        caption += filtername;

        gtk_window_set_title(GTK_WINDOW(picker), caption.c_str());

        gtk_widget_show_all(GTK_WIDGET(picker));
        gtk_main();

        rtn = (getFileCount() == 1);
    }

    gViewerWindow->getWindow()->afterDialog();

    return rtn;
}

bool LLFilePicker::getMultipleOpenFiles( ELoadFilter filter, bool blocking)
{
    bool rtn = false;

    // if local file browsing is turned off, return without opening dialog
    if (!check_local_file_access_enabled())
    {
        return false;
    }

    gViewerWindow->getWindow()->beforeDialog();

    reset();

    GtkWindow* picker = buildFilePicker(false, false, "openfile");

    if (picker)
    {
        gtk_file_chooser_set_select_multiple (GTK_FILE_CHOOSER(picker),
                              TRUE);

        gtk_window_set_title(GTK_WINDOW(picker), LLTrans::getString("load_files").c_str());

        gtk_widget_show_all(GTK_WIDGET(picker));
        gtk_main();
        rtn = !mFiles.empty();
    }

    gViewerWindow->getWindow()->afterDialog();

    return rtn;
}

#elif LL_FLTK


#if LL_GLIB
// <FS> Use the xdg-desktop-portal FileChooser when available. FLTK 1.3 can only
// show the ancient GTK2 dialog, the portal gives us the desktop's native one.
namespace
{
    // Turns an FLTK style filter ("Name \t*.{tga,png}" or "*.lsl") into portal
    // globs. Both lower and upper case variants are added, GTK3 globs are case sensitive.
    void fltk_filter_to_portal(const std::string& fltk_filter, const std::string& fallback_name,
                               std::string& name, std::vector<std::string>& globs)
    {
        std::string pattern = fltk_filter;
        name = fallback_name;

        size_t tab = fltk_filter.find('\t');
        if (tab != std::string::npos)
        {
            name = fltk_filter.substr(0, tab);
            LLStringUtil::trim(name);
            pattern = fltk_filter.substr(tab + 1);
        }
        LLStringUtil::trim(pattern);

        std::vector<std::string> patterns;
        size_t open = pattern.find('{');
        size_t close = pattern.find('}');
        if (open != std::string::npos && close != std::string::npos && close > open)
        {
            std::string prefix = pattern.substr(0, open);
            std::string suffix = pattern.substr(close + 1);
            std::string alternatives = pattern.substr(open + 1, close - open - 1);
            size_t start = 0;
            while (start <= alternatives.size())
            {
                size_t comma = alternatives.find(',', start);
                if (comma == std::string::npos)
                    comma = alternatives.size();
                patterns.push_back(prefix + alternatives.substr(start, comma - start) + suffix);
                start = comma + 1;
            }
        }
        else if (!pattern.empty())
        {
            patterns.push_back(pattern);
        }

        for (const std::string& p : patterns)
        {
            std::string lower = p, upper = p;
            LLStringUtil::toLower(lower);
            LLStringUtil::toUpper(upper);
            globs.push_back(lower);
            if (upper != lower)
                globs.push_back(upper);
        }
    }

    // All portal D-Bus callbacks are dispatched on this context, which the main loop
    // pumps every frame through LLFilePicker::updateModeless().
    GMainContext* portal_context()
    {
        static GMainContext* sContext = g_main_context_new();
        return sContext;
    }

    typedef std::function<void(bool portal_ok, std::vector<std::string>& files)> portal_done_t;

    struct PortalRequest
    {
        GDBusConnection* bus = nullptr;
        guint subscription = 0;
        portal_done_t done;
        bool save = false;
        bool directory = false;
        bool call_finished = false; // OpenFile/SaveFile method reply arrived
        bool answered = false;      // done() has been called

        ~PortalRequest()
        {
            if (subscription)
                g_dbus_connection_signal_unsubscribe(bus, subscription);
            if (bus)
                g_object_unref(bus);
        }

        void finish(bool portal_ok, std::vector<std::string>& files)
        {
            if (answered)
                return;
            answered = true;
            if (subscription)
            {
                g_dbus_connection_signal_unsubscribe(bus, subscription);
                subscription = 0;
            }
            done(portal_ok, files);
        }

        void maybeDelete()
        {
            if (answered && call_finished)
                delete this;
        }
    };

    void on_portal_response(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*,
                            GVariant* parameters, gpointer user_data)
    {
        PortalRequest* request = static_cast<PortalRequest*>(user_data);
        guint32 code = 2; // 0 = ok, 1 = cancelled, 2 = other
        GVariant* results = nullptr;
        g_variant_get(parameters, "(u@a{sv})", &code, &results);

        std::vector<std::string> files;
        if (code == 0 && results)
        {
            const gchar** uris = nullptr;
            if (g_variant_lookup(results, "uris", "^a&s", &uris) && uris)
            {
                for (const gchar** uri = uris; *uri; ++uri)
                {
                    gchar* path = g_filename_from_uri(*uri, nullptr, nullptr);
                    if (path)
                    {
                        files.push_back(path);
                        g_free(path);
                    }
                }
                g_free(uris);
            }
        }
        else if (code == 2)
        {
            LL_WARNS() << "File portal returned an error response" << LL_ENDL;
        }

        // Remember where the user picked from, so the next dialog opens there (FIRE-14924).
        if (!files.empty())
        {
            std::string folder = request->directory ? files.front() : gDirUtilp->getDirName(files.front());
            if (!folder.empty())
                gSavedSettings.setString(request->save ? "FSFilePickerSaveDirectory" : "FSFilePickerOpenDirectory", folder);
        }

        if (results)
            g_variant_unref(results);

        request->finish(true, files);
        request->maybeDelete();
    }

    void on_portal_call_finished(GObject* source, GAsyncResult* result, gpointer user_data)
    {
        PortalRequest* request = static_cast<PortalRequest*>(user_data);
        request->call_finished = true;

        GError* error = nullptr;
        GVariant* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
        if (reply)
        {
            g_variant_unref(reply);
        }
        else
        {
            LL_INFOS() << "File portal not available, falling back to FLTK: " << (error ? error->message : "unknown") << LL_ENDL;
            g_clear_error(&error);
            std::vector<std::string> files;
            request->finish(false, files);
        }
        request->maybeDelete();
    }

    // Asks the portal for a file dialog without blocking. done() is always called later from
    // the portal context, with portal_ok false if the portal could not be used (caller falls
    // back to FLTK). Returns false, without calling done(), if there is no session bus at all.
    bool portal_file_dialog_async(bool save, bool multiple, bool directory, const std::string& title,
                                  const std::string& filter_name, const std::vector<std::string>& globs,
                                  const std::string& proposed_name, portal_done_t done)
    {
        GError* error = nullptr;
        GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
        if (!bus)
        {
            LL_INFOS() << "No session bus for the file portal: " << (error ? error->message : "unknown") << LL_ENDL;
            g_clear_error(&error);
            return false;
        }

        PortalRequest* request = new PortalRequest;
        request->bus = bus;
        request->done = done;
        request->save = save;
        request->directory = directory;

        // Subscribe to the Response signal on the request path before calling, so we can't miss it.
        static U32 sRequestCounter = 0;
        std::string token = llformat("firestorm%u_%u", (U32)getpid(), ++sRequestCounter);
        std::string sender = g_dbus_connection_get_unique_name(bus) + 1; // skip leading ':'
        std::replace(sender.begin(), sender.end(), '.', '_');
        std::string request_path = "/org/freedesktop/portal/desktop/request/" + sender + "/" + token;

        g_main_context_push_thread_default(portal_context());

        request->subscription = g_dbus_connection_signal_subscribe(bus, "org.freedesktop.portal.Desktop",
            "org.freedesktop.portal.Request", "Response", request_path.c_str(), nullptr,
            G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE, on_portal_response, request, nullptr);

        GVariantBuilder options;
        g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(token.c_str()));
        g_variant_builder_add(&options, "{sv}", "modal", g_variant_new_boolean(TRUE));
        if (multiple)
            g_variant_builder_add(&options, "{sv}", "multiple", g_variant_new_boolean(TRUE));
        if (directory)
            g_variant_builder_add(&options, "{sv}", "directory", g_variant_new_boolean(TRUE));
        if (save && !proposed_name.empty())
            g_variant_builder_add(&options, "{sv}", "current_name", g_variant_new_string(proposed_name.c_str()));

        // Start in the last used folder instead of $HOME. The portal wants a NUL terminated byte string.
        std::string last_folder = gSavedSettings.getString(save ? "FSFilePickerSaveDirectory" : "FSFilePickerOpenDirectory");
        if (!last_folder.empty() && LLFile::isdir(last_folder))
            g_variant_builder_add(&options, "{sv}", "current_folder", g_variant_new_bytestring(last_folder.c_str()));

        if (!globs.empty())
        {
            GVariantBuilder patterns;
            g_variant_builder_init(&patterns, G_VARIANT_TYPE("a(us)"));
            for (const std::string& glob : globs)
                g_variant_builder_add(&patterns, "(us)", 0, glob.c_str());
            GVariant* filter = g_variant_ref_sink(g_variant_new("(sa(us))", filter_name.c_str(), &patterns));

            GVariantBuilder all_patterns;
            g_variant_builder_init(&all_patterns, G_VARIANT_TYPE("a(us)"));
            g_variant_builder_add(&all_patterns, "(us)", 0, "*");
            GVariant* all_filter = g_variant_new("(sa(us))", LLTrans::getString("all_files").c_str(), &all_patterns);

            GVariantBuilder filters;
            g_variant_builder_init(&filters, G_VARIANT_TYPE("a(sa(us))"));
            g_variant_builder_add_value(&filters, filter);
            g_variant_builder_add_value(&filters, all_filter);
            g_variant_builder_add(&options, "{sv}", "filters", g_variant_builder_end(&filters));
            g_variant_builder_add(&options, "{sv}", "current_filter", filter);
            g_variant_unref(filter);
        }

        std::string parent;
#if LL_SDL && LL_X11
        // Only parent on a pure X11 session. Under Wayland the viewer runs through XWayland
        // and an x11 parent makes the GTK portal open its dialog on XWayland too, where the
        // compositor can't see the CSD shadow extents and draws its border around the shadow.
        const char* wayland_display = getenv("WAYLAND_DISPLAY");
        Window xwindow = LLWindowSDL::get_SDL_XWindowID();
        if (xwindow != None && (!wayland_display || !*wayland_display))
            parent = llformat("x11:%lx", (unsigned long)xwindow);
#endif

        g_dbus_connection_call(bus, "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop", "org.freedesktop.portal.FileChooser",
            save ? "SaveFile" : "OpenFile",
            g_variant_new("(ssa{sv})", parent.c_str(), title.c_str(), &options),
            G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr,
            on_portal_call_finished, request);

        g_main_context_pop_thread_default(portal_context());
        return true;
    }

    // Blocking variant for callers that still use the modal LLFilePicker API.
    // Returns false if the portal is not usable, so the caller can fall back to FLTK.
    bool portal_file_dialog(bool save, bool multiple, bool directory, const std::string& title,
                            const std::string& filter_name, const std::vector<std::string>& globs,
                            const std::string& proposed_name, std::vector<std::string>& files)
    {
        bool finished = false;
        bool portal_ok = false;
        bool started = portal_file_dialog_async(save, multiple, directory, title, filter_name, globs, proposed_name,
            [&](bool ok, std::vector<std::string>& result)
            {
                finished = true;
                portal_ok = ok;
                files = result;
            });
        if (!started)
            return false;

        while (!finished)
            g_main_context_iteration(portal_context(), TRUE);
        return portal_ok;
    }
}
// </FS>
#endif // LL_GLIB

//static
void LLFilePicker::updateModeless()
{
#if LL_GLIB
    // Dispatch pending portal replies, this is what invokes the modeless callbacks.
    while (g_main_context_iteration(portal_context(), FALSE))
    {
    }
#endif
}

// <FS> Modeless dialogs go through the xdg-desktop-portal so the viewer keeps running
// while the dialog is open. Without a portal we fall back to the blocking FLTK dialog.
void LLFilePicker::openFileDialogModeless(int32_t filter, EType aType, const std::string& proposed_name,
                                          std::function<void(bool, std::vector<std::string>&)> done)
{
    if (!LLFilePicker().check_local_file_access_enabled())
    {
        std::vector<std::string> files;
        done(false, files);
        return;
    }

    auto fallback = [filter, aType, proposed_name, done]()
    {
        LLFilePicker picker;
        picker.fltkFileDialog(filter, aType, proposed_name);
        done(!picker.mFiles.empty(), picker.mFiles);
    };

#if LL_GLIB
    std::string title, fltk_filter;
    getDialogStrings(filter, aType, title, fltk_filter);

    std::string filter_name;
    std::vector<std::string> globs;
    if (!fltk_filter.empty())
    {
        fltk_filter_to_portal(fltk_filter, title, filter_name, globs);
    }

    bool started = portal_file_dialog_async(aType == eSaveFile, aType == eOpenMultiple,
        aType != eSaveFile && filter == FFLOAD_DIRECTORY, title, filter_name, globs, proposed_name,
        [fallback, done](bool portal_ok, std::vector<std::string>& files)
        {
            if (portal_ok)
                done(!files.empty(), files);
            else
                fallback();
        });
    if (started)
        return;
#endif

    fallback();
}
// </FS>

bool LLFilePicker::getOpenFileModeless(ELoadFilter filter,
                                       void (*callback)(bool, std::vector<std::string> &, void*),
                                       void *userdata)
{
    openFileDialogModeless(filter, eOpenFile, LLStringUtil::null,
        [callback, userdata](bool success, std::vector<std::string>& files) { callback(success, files, userdata); });
    return true;
}

bool LLFilePicker::getMultipleOpenFilesModeless(ELoadFilter filter,
                                                void (*callback)(bool, std::vector<std::string> &, void*),
                                                void *userdata )
{
    openFileDialogModeless(filter, eOpenMultiple, LLStringUtil::null,
        [callback, userdata](bool success, std::vector<std::string>& files) { callback(success, files, userdata); });
    return true;
}

bool LLFilePicker::getSaveFileModeless(ESaveFilter filter,
                                       const std::string& filename,
                                       void (*callback)(bool, std::string&, void*),
                                       void *userdata)
{
    openFileDialogModeless(filter, eSaveFile, filename,
        [callback, userdata](bool success, std::vector<std::string>& files)
        {
            std::string file = files.empty() ? std::string() : files.front();
            callback(success, file, userdata);
        });
    return true;
}

bool LLFilePicker::getSaveFile( ESaveFilter filter, const std::string& filename, bool blocking )
{
    return openFileDialog( filter, blocking, eSaveFile, filename );
}

bool LLFilePicker::getOpenFile( ELoadFilter filter, bool blocking )
{
    return openFileDialog( filter, blocking, eOpenFile );
}

bool LLFilePicker::getMultipleOpenFiles( ELoadFilter filter, bool blocking)
{
    return openFileDialog( filter, blocking, eOpenMultiple );
}

//static
void LLFilePicker::getDialogStrings( int32_t filter, EType aType, std::string& file_dialog_title, std::string& file_dialog_filter )
{
    file_dialog_title.clear();
    file_dialog_filter.clear();

    if (aType == EType::eSaveFile)
    {
        std::string file_type("all_files");

        switch ((ESaveFilter) filter)
        {
            case FFSAVE_ALL:
                break;
            case FFSAVE_TGA:
                file_type = "targa_image_files";
                file_dialog_filter = "*.tga";
                break;
            case FFSAVE_BMP:
                file_type = "bitmap_image_files";
                file_dialog_filter = "*.bmp";
                break;
            case FFSAVE_AVI:
                file_type = "avi_movie_file";
                file_dialog_filter = "*.avi";
                break;
            case FFSAVE_ANIM:
                file_type = "xaf_animation_file";
                file_dialog_filter = "*.xaf";
                break;
            case FFSAVE_XML:
                file_type = "xml_file";
                file_dialog_filter = "*.xml";
                break;
            case FFSAVE_COLLADA:
                file_type = "collada_files";
                file_dialog_filter = "*.dae";
                break;
            case FFSAVE_RAW:
                file_type = "raw_file";
                file_dialog_filter = "*.raw";
                break;
            case FFSAVE_J2C:
                file_type = "compressed_image_files";
                file_dialog_filter = "*.j2c";
                break;
            case FFSAVE_PNG:
                file_type = "png_image_files";
                file_dialog_filter = "*.png";
                break;
            case FFSAVE_JPEG:
                file_type = "jpeg_image_files";
                file_dialog_filter = "*.{jpg,jpeg}";
                break;
            case FFSAVE_SCRIPT:
                file_type = "script_files";
                file_dialog_filter = "*.lsl";
                break;
            case FFSAVE_TGAPNG:
                file_type = "save_texture_image_files";
                file_dialog_filter = "*.{tga,png}";
                break;
            case FFSAVE_WAV:
                file_type = "sound_files";
                file_dialog_filter = "*.wav";
                break;

            // <FS:Zi> Handle all enums in a switch, or you make GCC unhappy
            case FFSAVE_GLTF:
                file_type = "gltf_files";
                file_dialog_filter = "*.{gltf,glb}";
                break;
            // </FS:Zi>

            // Firestorm additions
            case FFSAVE_BEAM:
                file_type = "xml_file";
                file_dialog_filter = "*.xml";
                break;
            case FFSAVE_EXPORT:
                file_type = "backup_files";
                file_dialog_filter = "*.oxp";
                break;
            case FFSAVE_CSV:
                file_type = "csv_files";
                file_dialog_filter = "*.csv";
                break;

#ifdef _CORY_TESTING
            case FFSAVE_GEOMETRY:
                // no file type translation for this, so using the default "all_files" for now
                file_dialog_filter = "*.slg";
                break;
#endif
        }

        // can't say I like this combining of verb+type, it might not work too well in all languages -Zi
        file_dialog_title = LLTrans::getString("save_file_verb") + " " + LLTrans::getString(file_type);
        file_dialog_filter = LLTrans::getString(file_type) + " \t" + file_dialog_filter;
    }
    else
    {
        std::string file_type("all_files");

        switch ((ELoadFilter) filter)
        {
            case FFLOAD_ALL:
                break;
            case FFLOAD_WAV:
                file_type = "sound_files";
                file_dialog_filter = "*.wav";
                break;
            case FFLOAD_IMAGE:
                file_type = "image_files";
                file_dialog_filter = "*.{tga,bmp,jpg,jpeg,png}";
                break;
            case FFLOAD_ANIM:
                file_type = "animation_files";
                file_dialog_filter = "*.{bvh,anim}";
                break;
            case FFLOAD_XML:
                file_type = "xml_file";
                file_dialog_filter = "*.xml";
                break;
            case FFLOAD_SLOBJECT:
                file_type = "xml_file";
                file_dialog_filter = "*.slobject";
                break;
            case FFLOAD_RAW:
                file_type = "raw_file";
                file_dialog_filter = "*.raw";
                break;
            case FFLOAD_MODEL:
                file_type = "model_files";
                file_dialog_filter = "*.{dae,gltf,glb}";
                break;
            case FFLOAD_COLLADA:
                file_type = "collada_files";
                file_dialog_filter = "*.dae";
                break;
            case FFLOAD_SCRIPT:
                file_type = "script_files";
                file_dialog_filter = "*.lsl";
                break;
            case FFLOAD_DICTIONARY:
                file_type = "dictionary_files";
                file_dialog_filter = "*.{dic,xcu}";
                break;
            case FFLOAD_DIRECTORY:
                file_type = "choose_the_directory";
                break;
            case FFLOAD_EXE:
                file_type = "executable_files";
                break;

            // <FS:Zi> Handle all enums in a switch, or you make GCC unhappy
            case FFLOAD_GLTF:
                file_type = "gltf_files";
                file_dialog_filter = "*.{gltf,glb}";
                break;
            case FFLOAD_MATERIAL:
                file_type = "material_files";
                file_dialog_filter = "*.{gltf,glb}";
                break;
            case FFLOAD_HDRI:
                file_type = "hdri_files";
                file_dialog_filter = "*.{exr}";
                break;
            case FFLOAD_MATERIAL_TEXTURE:
                file_type = "material_texture_files";
                file_dialog_filter = "*.{gltf,glb,tga,bmp,jpg,jpeg,png}";
                break;
            // </FS:Zi>

            // Firestorm additions
            case FFLOAD_IMPORT:
                file_type = "backup_files";
                file_dialog_filter = "*.oxp";
                break;

#ifdef _CORY_TESTING
            case FFLOAD_GEOMETRY:
                // no file type translation for this, so using the default "all_files" for now
                file_dialog_filter = "*.slg";
                break;
#endif
        }

        if (aType == EType::eOpenMultiple)
        {
            file_dialog_title = LLTrans::getString("load_files");
        }
        else
        {
            // can't say I like this combining of verb+type, it might not work too well in all languages -Zi
            file_dialog_title = LLTrans::getString("load_file_verb") + " " + LLTrans::getString(file_type);
            file_dialog_filter = LLTrans::getString(file_type) + " \t" + file_dialog_filter;
        }
    }

}

bool LLFilePicker::openFileDialog( int32_t filter, bool blocking, EType aType, const std::string& proposed_name )
{
    if ( check_local_file_access_enabled() == false )
        return false;

#if LL_GLIB
    std::string file_dialog_title;
    std::string file_dialog_filter;
    getDialogStrings(filter, aType, file_dialog_title, file_dialog_filter);

    std::string filter_name;
    std::vector<std::string> globs;
    if (!file_dialog_filter.empty())
    {
        fltk_filter_to_portal(file_dialog_filter, file_dialog_title, filter_name, globs);
    }

    reset();
    if (portal_file_dialog(aType == eSaveFile, aType == eOpenMultiple,
                           aType != eSaveFile && filter == FFLOAD_DIRECTORY,
                           file_dialog_title, filter_name, globs, proposed_name, mFiles))
    {
        return !mFiles.empty();
    }
#endif

    return fltkFileDialog(filter, aType, proposed_name);
}

bool LLFilePicker::fltkFileDialog( int32_t filter, EType aType, const std::string& proposed_name )
{
    gViewerWindow->getWindow()->beforeDialog();
    reset();
    Fl_Native_File_Chooser::Type flType = Fl_Native_File_Chooser::BROWSE_FILE;

    if( aType == eOpenMultiple )
        flType = Fl_Native_File_Chooser::BROWSE_MULTI_FILE;
    else if( aType == eSaveFile )
        flType = Fl_Native_File_Chooser::BROWSE_SAVE_FILE;

    Fl_Native_File_Chooser flDlg;

    std::string file_dialog_title;
    std::string file_dialog_filter;
    getDialogStrings(filter, aType, file_dialog_title, file_dialog_filter);

    flDlg.title(file_dialog_title.c_str());
    flDlg.type(flType);
    if (!proposed_name.empty())
    {
        flDlg.preset_file(proposed_name.c_str());
    }

    if (!file_dialog_filter.empty())
    {
        flDlg.filter(file_dialog_filter.c_str());
    }

    int res = flDlg.show();
    gViewerWindow->getWindow()->afterDialog();

    if( res == 0 )
    {
        int32_t count = flDlg.count();
        if( count < 0 )
            count = 0;
        for( int32_t i = 0; i < count; ++i )
        {
            char const *pFile = flDlg.filename(i);
            if( pFile && strlen(pFile) > 0 )
                mFiles.push_back( pFile  );
        }
    }
    else if( res == -1 )
    {
        LL_WARNS() << "FLTK failed: " <<  flDlg.errmsg() << LL_ENDL;
    }

    return mFiles.empty() ? false : true;
}

# else // LL_GTK

// Hacky stubs designed to facilitate fake getSaveFile and getOpenFile with
// static results, when we don't have a real filepicker.

bool LLFilePicker::getSaveFile( ESaveFilter filter, const std::string& filename, bool blocking )
{
    // if local file browsing is turned off, return without opening dialog
    // (Even though this is a stub, I think we still should not return anything at all)
    if (!check_local_file_access_enabled())
    {
        return false;
    }

    reset();

    LL_INFOS() << "getSaveFile suggested filename is [" << filename
        << "]" << LL_ENDL;
    if (!filename.empty())
    {
        mFiles.push_back(gDirUtilp->getLindenUserDir() + gDirUtilp->getDirDelimiter() + filename);
        return true;
    }
    return false;
}

bool LLFilePicker::getSaveFileModeless(ESaveFilter filter,
                                       const std::string& filename,
                                       void (*callback)(bool, std::string&, void*),
                                       void *userdata)
{
    LL_ERRS() << "NOT IMPLEMENTED" << LL_ENDL;
    return false;
}

bool LLFilePicker::getOpenFile( ELoadFilter filter, bool blocking )
{
    // if local file browsing is turned off, return without opening dialog
    // (Even though this is a stub, I think we still should not return anything at all)
    if (!check_local_file_access_enabled())
    {
        return false;
    }

    reset();

    // HACK: Static filenames for 'open' until we implement filepicker
    std::string filename = gDirUtilp->getLindenUserDir() + gDirUtilp->getDirDelimiter() + "upload";
    switch (filter)
    {
    case FFLOAD_WAV: filename += ".wav"; break;
    case FFLOAD_IMAGE: filename += ".tga"; break;
    case FFLOAD_ANIM: filename += ".bvh"; break;
    default: break;
    }
    mFiles.push_back(filename);
    LL_INFOS() << "getOpenFile: Will try to open file: " << filename << LL_ENDL;
    return true;
}

bool LLFilePicker::getOpenFileModeless(ELoadFilter filter,
                                       void (*callback)(bool, std::vector<std::string> &, void*),
                                       void *userdata)
{
    LL_ERRS() << "NOT IMPLEMENTED" << LL_ENDL;
    return false;
}

bool LLFilePicker::getMultipleOpenFiles( ELoadFilter filter, bool blocking)
{
    // if local file browsing is turned off, return without opening dialog
    // (Even though this is a stub, I think we still should not return anything at all)
    if (!check_local_file_access_enabled())
    {
        return false;
    }

    reset();
    return false;
}

bool LLFilePicker::getMultipleOpenFilesModeless(ELoadFilter filter,
                                                void (*callback)(bool, std::vector<std::string> &, void*),
                                                void *userdata )
{
    LL_ERRS() << "NOT IMPLEMENTED" << LL_ENDL;
    return false;
}

#endif // LL_GTK

#else // not implemented

bool LLFilePicker::getSaveFile( ESaveFilter filter, const std::string& filename, bool blocking )
{
    reset();
    return false;
}

bool LLFilePicker::getOpenFile( ELoadFilter filter )
{
    reset();
    return false;
}

bool LLFilePicker::getMultipleOpenFiles( ELoadFilter filter, bool blocking)
{
    reset();
    return false;
}

#endif // LL_LINUX
