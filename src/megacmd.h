/**
 * @file src/megacmd.h
 * @brief MEGAcmd: Interactive CLI and service application
 *
 * (c) 2013-2016 by Mega Limited, Auckland, New Zealand
 *
 * This file is part of the MEGAcmd.
 *
 * MEGAcmd is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * @copyright Simplified (2-clause) BSD License.
 *
 * You should have received a copy of the license along with this
 * program.
 */

#ifndef MEGACMD_H
#define MEGACMD_H

#ifdef _WIN32

#define OUTSTREAMTYPE std::wostream
#define OUTSTRINGSTREAM std::wostringstream
#define OUTSTRING std::wstring
#define COUT wcout



#include <string>
std::wostream & operator<< ( std::wostream & ostr, std::string const & str );
std::wostream & operator<< ( std::wostream & ostr, const char * str );
std::ostringstream & operator<< ( std::ostringstream & ostr, std::wstring const &str);

void localwtostring(const std::wstring* wide, std::string *multibyte);

#else
#define OUTSTREAMTYPE std::ostream
#define OUTSTRINGSTREAM std::ostringstream
#define OUTSTRING std::string
#define COUT std::cout
#endif

#include "megaapi_impl.h"

#define PROGRESS_COMPLETE -2

typedef struct sync_struct
{
    mega::MegaHandle handle;
    bool active;
    std::string localpath;
    long long fingerprint;
    bool loadedok; //ephimeral data
} sync_struct;


typedef struct backup_struct
{
    mega::MegaHandle handle;
    bool active;
    std::string localpath; //TODO: review wether this is local or utf-8 representation and be consistent
    int64_t period;
    std::string speriod;
    int numBackups;
    bool failed; //This should mark the failure upon resuming. It shall not be persisted
    int tag; //This is depends on execution. should not be persisted
    int id; //Internal id for megacmd. Depends on execution should not be persisted
} backup_istruct;


enum prompttype
{
    COMMAND, LOGINPASSWORD, OLDPASSWORD, NEWPASSWORD, PASSWORDCONFIRM, AREYOUSURETODELETE
};

static const char* const prompts[] =
{
    "MEGA CMD> ", "Password:", "Old Password:", "New Password:", "Retype New Password:", "Are you sure to delete? "
};

enum
{
    MCMD_OK = 0,              ///< Everything OK

    MCMD_EARGS = -51,         ///< Wrong arguments
    MCMD_INVALIDEMAIL = -52,  ///< Invalid email
    MCMD_NOTFOUND = -53,      ///< Resource not found
    MCMD_INVALIDSTATE = -54,  ///< Invalid state
    MCMD_INVALIDTYPE = -55,   ///< Invalid type
    MCMD_NOTPERMITTED = -56,  ///< Operation not allowed
    MCMD_NOTLOGGEDIN = -57,   ///< Needs loging in
    MCMD_NOFETCH = -58,       ///< Nodes not fetched
    MCMD_EUNEXPECTED = -59,   ///< Unexpected failure

    MCMD_REQCONFIRM = -60,     ///< Confirmation required

};


enum confirmresponse
{
    MCMDCONFIRM_NO=0,
    MCMDCONFIRM_YES,
    MCMDCONFIRM_ALL,
    MCMDCONFIRM_NONE
};

void changeprompt(const char *newprompt);

mega::MegaApi* getFreeApiFolder();
void freeApiFolder(mega::MegaApi *apiFolder);

const char * getUsageStr(const char *command);

void unescapeifRequired(std::string &what);

void setprompt(prompttype p, std::string arg = "");

prompttype getprompt();

void printHistory();

int askforConfirmation(std::string message);

void informTransferUpdate(mega::MegaTransfer *transfer, int clientID);

void informProgressUpdate(long long transferred, long long total, int clientID, std::string title = "");



#endif
