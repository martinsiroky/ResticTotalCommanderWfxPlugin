// fsplugin.h - Total Commander WFX Plugin SDK
// Based on version 2.1 (27.April.2010)
// Source: https://ghisler.github.io/WFX-SDK/

#ifndef FSPLUGIN_H
#define FSPLUGIN_H

#include <windows.h>

// ids for FsGetFile
#define FS_FILE_OK 0
#define FS_FILE_EXISTS 1
#define FS_FILE_NOTFOUND 2
#define FS_FILE_READERROR 3
#define FS_FILE_WRITEERROR 4
#define FS_FILE_USERABORT 5
#define FS_FILE_NOTSUPPORTED 6
#define FS_FILE_EXISTSRESUMEALLOWED 7

#define FS_EXEC_OK 0
#define FS_EXEC_ERROR 1
#define FS_EXEC_YOURSELF -1
#define FS_EXEC_SYMLINK -2

#define FS_COPYFLAGS_OVERWRITE 1
#define FS_COPYFLAGS_RESUME 2
#define FS_COPYFLAGS_MOVE 4
#define FS_COPYFLAGS_EXISTS_SAMECASE 8
#define FS_COPYFLAGS_EXISTS_DIFFERENTCASE 16

// flags for tRequestProc
#define RT_Other 0
#define RT_UserName 1
#define RT_Password 2
#define RT_Account 3
#define RT_UserNameFirewall 4
#define RT_PasswordFirewall 5
#define RT_TargetDir 6
#define RT_URL 7
#define RT_MsgOK 8
#define RT_MsgYesNo 9
#define RT_MsgOKCancel 10

// flags for tLogProc
#define MSGTYPE_CONNECT 1
#define MSGTYPE_DISCONNECT 2
#define MSGTYPE_DETAILS 3
#define MSGTYPE_TRANSFERCOMPLETE 4
#define MSGTYPE_CONNECTCOMPLETE 5
#define MSGTYPE_IMPORTANTERROR 6
#define MSGTYPE_OPERATIONCOMPLETE 7

// flags for FsStatusInfo
#define FS_STATUS_START 0
#define FS_STATUS_END 1
#define FS_STATUS_OP_LIST 1
#define FS_STATUS_OP_GET_SINGLE 2
#define FS_STATUS_OP_GET_MULTI 3
#define FS_STATUS_OP_PUT_SINGLE 4
#define FS_STATUS_OP_PUT_MULTI 5
#define FS_STATUS_OP_RENMOV_SINGLE 6
#define FS_STATUS_OP_RENMOV_MULTI 7
#define FS_STATUS_OP_DELETE 8
#define FS_STATUS_OP_ATTRIB 9
#define FS_STATUS_OP_MKDIR 10
#define FS_STATUS_OP_EXEC 11
#define FS_STATUS_OP_CALCSIZE 12
#define FS_STATUS_OP_SEARCH 13
#define FS_STATUS_OP_SEARCH_TEXT 14
#define FS_STATUS_OP_SYNC_SEARCH 15
#define FS_STATUS_OP_SYNC_GET 16
#define FS_STATUS_OP_SYNC_PUT 17
#define FS_STATUS_OP_SYNC_DELETE 18
#define FS_STATUS_OP_GET_MULTI_THREAD 19
#define FS_STATUS_OP_PUT_MULTI_THREAD 20

#define FS_ICONFLAG_SMALL 1
#define FS_ICONFLAG_BACKGROUND 2
#define FS_ICON_USEDEFAULT 0
#define FS_ICON_EXTRACTED 1
#define FS_ICON_EXTRACTED_DESTROY 2
#define FS_ICON_DELAYED 3

#define FS_BITMAP_NONE 0
#define FS_BITMAP_EXTRACTED 1
#define FS_BITMAP_EXTRACT_YOURSELF 2
#define FS_BITMAP_EXTRACT_YOURSELF_ANDDELETE 3
#define FS_BITMAP_CACHE 256

#define FS_CRYPT_SAVE_PASSWORD 1
#define FS_CRYPT_LOAD_PASSWORD 2
#define FS_CRYPT_LOAD_PASSWORD_NO_UI 3
#define FS_CRYPT_COPY_PASSWORD 4
#define FS_CRYPT_MOVE_PASSWORD 5
#define FS_CRYPT_DELETE_PASSWORD 6

#define FS_CRYPTOPT_MASTERPASS_SET 1

#define BG_DOWNLOAD 1
#define BG_UPLOAD 2
#define BG_ASK_USER 4

// Content plugin field types (for FsContentGetSupportedField)
#define ft_nomorefields      0
#define ft_numeric_32        1
#define ft_numeric_64        2
#define ft_numeric_floating  3
#define ft_date              4
#define ft_time              5
#define ft_boolean           6
#define ft_multiplechoice    7
#define ft_string            8
#define ft_fulltext          9
#define ft_datetime         10

// Content plugin return codes (for FsContentGetValue)
#define ft_nosuchfield      -1
#define ft_fileerror        -2
#define ft_fieldempty       -3

#define CONTENT_DELAYIFSLOW  1

typedef struct {
    DWORD SizeLow, SizeHigh;
    FILETIME LastWriteTime;
    int Attr;
} RemoteInfoStruct;

typedef struct {
    int size;
    DWORD PluginInterfaceVersionLow;
    DWORD PluginInterfaceVersionHi;
    char DefaultIniName[MAX_PATH];
} FsDefaultParamStruct;

// callback functions
typedef int (__stdcall *tProgressProc)(int PluginNr, char* SourceName,
    char* TargetName, int PercentDone);
typedef int (__stdcall *tProgressProcW)(int PluginNr, WCHAR* SourceName,
    WCHAR* TargetName, int PercentDone);
typedef void (__stdcall *tLogProc)(int PluginNr, int MsgType, char* LogString);
typedef void (__stdcall *tLogProcW)(int PluginNr, int MsgType, WCHAR* LogString);
typedef BOOL (__stdcall *tRequestProc)(int PluginNr, int RequestType,
    char* CustomTitle, char* CustomText, char* ReturnedText, int maxlen);
typedef BOOL (__stdcall *tRequestProcW)(int PluginNr, int RequestType,
    WCHAR* CustomTitle, WCHAR* CustomText, WCHAR* ReturnedText, int maxlen);
typedef int (__stdcall *tCryptProc)(int PluginNr, int CryptoNr, int Mode,
    char* ConnectionName, char* Password, int maxlen);
typedef int (__stdcall *tCryptProcW)(int PluginNr, int CryptoNr, int Mode,
    WCHAR* ConnectionName, WCHAR* Password, int maxlen);

#endif // FSPLUGIN_H
