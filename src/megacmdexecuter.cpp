/**
 * @file src/megacmdexecuter.cpp
 * @brief MEGAcmd: Executer of the commands
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

#include "megacmdexecuter.h"
#include "megacmd.h"

#include "megacmdutils.h"
#include "configurationmanager.h"
#include "megacmdlogger.h"
#include "comunicationsmanager.h"
#include "listeners.h"
#include "megacmdversion.h"

#include <iomanip>
#include <string>
#include <ctime>

#include <set>

#include <signal.h>

using namespace mega;

static const char* rootnodenames[] = { "ROOT", "INBOX", "RUBBISH" };
static const char* rootnodepaths[] = { "/", "//in", "//bin" };

#define SSTR( x ) static_cast< const std::ostringstream & >( \
        ( std::ostringstream() << std::dec << x ) ).str()

/**
 * @brief updateprompt updates prompt with the current user/location
 * @param api
 * @param handle
 */
void MegaCmdExecuter::updateprompt(MegaApi *api, MegaHandle handle)
{
    static char dynamicprompt[2024]; //TODO: rewrite this function to have prompts with any size (use string)

    MegaNode *n = api->getNodeByHandle(handle);

    MegaUser *u = api->getMyUser();
    char *ptraux = dynamicprompt;
    char *lastpos = dynamicprompt + sizeof( dynamicprompt ) / sizeof( dynamicprompt[0] );
    if (u)
    {
        const char *email = u->getEmail();
        strncpy(dynamicprompt, email, ( lastpos - ptraux ) / sizeof( dynamicprompt[0] ));
        ptraux += strlen(email);
        ptraux = std::min(ptraux, lastpos - 2);
        delete u;
    }
    if (n)
    {
        char *np = api->getNodePath(n);
        if (ptraux!=dynamicprompt)
            *ptraux++ = ':';
        ptraux = std::min(ptraux, lastpos - 2);
        strncpy(ptraux, np, ( lastpos - ptraux ) / sizeof( dynamicprompt[0] ));
        ptraux += strlen(np);
        ptraux = std::min(ptraux, lastpos - 2);
        delete n;
        delete []np;
    }
    if (ptraux == dynamicprompt)
    {
        strcpy(ptraux, prompts[0]);
    }
    else
    {
        *ptraux++ = '$';
        ptraux = std::min(ptraux, lastpos - 1);

        *ptraux++ = ' ';
        ptraux = std::min(ptraux, lastpos);

        *ptraux = '\0';
    }

    changeprompt(dynamicprompt);
}

MegaCmdExecuter::MegaCmdExecuter(MegaApi *api, MegaCMDLogger *loggerCMD, MegaCmdSandbox *sandboxCMD)
{
    signingup = false;
    confirming = false;

    this->api = api;
    this->loggerCMD = loggerCMD;
    this->sandboxCMD = sandboxCMD;
    this->globalTransferListener = new MegaCmdGlobalTransferListener(api, sandboxCMD);
    api->addTransferListener(globalTransferListener);
    cwd = UNDEF;
    fsAccessCMD = new MegaFileSystemAccess();
    mtxSyncMap.init(false);
    mtxWebDavLocations.init(false);
#ifdef ENABLE_BACKUPS
    mtxBackupsMap.init(true);
#endif
    session = NULL;
}

MegaCmdExecuter::~MegaCmdExecuter()
{
    delete fsAccessCMD;
    delete []session;
    for (std::vector< MegaNode * >::iterator it = nodesToConfirmDelete.begin(); it != nodesToConfirmDelete.end(); ++it)
    {
        delete *it;
    }
    nodesToConfirmDelete.clear();
    delete globalTransferListener;
}

// list available top-level nodes and contacts/incoming shares
void MegaCmdExecuter::listtrees()
{
    for (int i = 0; i < (int)( sizeof rootnodenames / sizeof *rootnodenames ); i++)
    {
        OUTSTREAM << rootnodenames[i] << " on " << rootnodepaths[i] << std::endl;
        if (!api->isLoggedIn())
        {
            break;                     //only show /root
        }
    }

    MegaShareList * msl = api->getInSharesList();
    for (int i = 0; i < msl->size(); i++)
    {
        MegaShare *share = msl->get(i);
        MegaNode *n = api->getNodeByHandle(share->getNodeHandle());

        OUTSTREAM << "INSHARE on " << share->getUser() << ":" << n->getName() << " (" << getAccessLevelStr(share->getAccess()) << ")" << std::endl;
        delete n;
    }

    delete ( msl );
}

bool MegaCmdExecuter::includeIfIsExported(MegaApi *api, MegaNode * n, void *arg)
{
    if (n->isExported())
    {
        (( vector<MegaNode*> * )arg )->push_back(n->copy());
        return true;
    }
    return false;
}

bool MegaCmdExecuter::includeIfIsShared(MegaApi *api, MegaNode * n, void *arg)
{
    if (n->isShared())
    {
        (( vector<MegaNode*> * )arg )->push_back(n->copy());
        return true;
    }
    return false;
}

bool MegaCmdExecuter::includeIfIsPendingOutShare(MegaApi *api, MegaNode * n, void *arg)
{
    MegaShareList* pendingoutShares = api->getPendingOutShares(n);
    if (pendingoutShares && pendingoutShares->size())
    {
        (( vector<MegaNode*> * )arg )->push_back(n->copy());
        return true;
    }
    if (pendingoutShares)
    {
        delete pendingoutShares;
    }
    return false;
}


bool MegaCmdExecuter::includeIfIsSharedOrPendingOutShare(MegaApi *api, MegaNode * n, void *arg)
{
    if (n->isShared())
    {
        (( vector<MegaNode*> * )arg )->push_back(n->copy());
        return true;
    }
    MegaShareList* pendingoutShares = api->getPendingOutShares(n);
    if (pendingoutShares && pendingoutShares->size())
    {
        (( vector<MegaNode*> * )arg )->push_back(n->copy());
        return true;
    }
    if (pendingoutShares)
    {
        delete pendingoutShares;
    }
    return false;
}

struct patternNodeVector
{
    string pattern;
    bool usepcre;
    vector<MegaNode*> *nodesMatching;
};

struct criteriaNodeVector
{
    string pattern;
    bool usepcre;
    time_t minTime;
    time_t maxTime;

    int64_t maxSize;
    int64_t minSize;

    vector<MegaNode*> *nodesMatching;
};

bool MegaCmdExecuter::includeIfMatchesPattern(MegaApi *api, MegaNode * n, void *arg)
{
    struct patternNodeVector *pnv = (struct patternNodeVector*)arg;
    if (patternMatches(n->getName(), pnv->pattern.c_str(), pnv->usepcre))
    {
        pnv->nodesMatching->push_back(n->copy());
        return true;
    }
    return false;
}


bool MegaCmdExecuter::includeIfMatchesCriteria(MegaApi *api, MegaNode * n, void *arg)
{
    struct criteriaNodeVector *pnv = (struct criteriaNodeVector*)arg;

    if ( pnv->maxTime != -1 && (n->getModificationTime() >= pnv->maxTime) )
    {
        return false;
    }
    if ( pnv->minTime != -1 && (n->getModificationTime() <= pnv->minTime) )
    {
        return false;
    }

    if ( pnv->maxSize != -1 && (n->getType() != MegaNode::TYPE_FILE || (n->getSize() > pnv->maxSize) ) )
    {
        return false;
    }

    if ( pnv->minSize != -1 && (n->getType() != MegaNode::TYPE_FILE || (n->getSize() < pnv->minSize) ) )
    {
        return false;
    }

    if (!patternMatches(n->getName(), pnv->pattern.c_str(), pnv->usepcre))
    {
        return false;
    }

    pnv->nodesMatching->push_back(n->copy());
    return true;
}

bool MegaCmdExecuter::processTree(MegaNode *n, bool processor(MegaApi *, MegaNode *, void *), void *( arg ))
{
    if (!n)
    {
        return false;
    }
    bool toret = true;
    MegaNodeList *children = api->getChildren(n);
    if (children)
    {
        for (int i = 0; i < children->size(); i++)
        {
            bool childret = processTree(children->get(i), processor, arg);
            toret = toret && childret;
        }

        delete children;
    }

    bool currentret = processor(api, n, arg);
    return toret && currentret;
}


// returns node pointer determined by path relative to cwd
// path naming conventions:
// * path is relative to cwd
// * /path is relative to ROOT
// * //in is in INBOX
// * //bin is in RUBBISH
// * X: is user X's INBOX
// * X:SHARE is share SHARE from user X
// * : and / filename components, as well as the \, must be escaped by \.
// (correct UTF-8 encoding is assumed)
// returns NULL if path malformed or not found
MegaNode* MegaCmdExecuter::nodebypath(const char* ptr, string* user, string* namepart)
{
    vector<string> c;
    string s;
    int l = 0;
    const char* bptr = ptr;
    int remote = 0;
    MegaNode* n = NULL;
    MegaNode* nn = NULL;

    if (*ptr == '\0')
    {
        LOG_warn << "Trying to get node whose path is \"\"";
        return NULL;
    }
    // split path by / or :
    do
    {
        if (!l)
        {
            if (*(const signed char*)ptr >= 0)
            {
                if (*ptr == '\\')
                {
                    if (ptr > bptr)
                    {
                        s.append(bptr, ptr - bptr);
                    }

                    bptr = ++ptr;

                    if (*bptr == 0)
                    {
                        c.push_back(s);
                        break;
                    }

                    ptr++;
                    continue;
                }

                if (( *ptr == '/' ) || ( *ptr == ':' ) || !*ptr)
                {
                    if (*ptr == ':')
                    {
                        if (c.size())
                        {
                            return NULL;
                        }

                        remote = 1;
                    }

                    if (ptr > bptr)
                    {
                        s.append(bptr, ptr - bptr);
                    }

                    bptr = ptr + 1;

                    c.push_back(s);

                    s.erase();
                }
            }
            else if (( *ptr & 0xf0 ) == 0xe0)
            {
                l = 1;
            }
            else if (( *ptr & 0xf8 ) == 0xf0)
            {
                l = 2;
            }
            else if (( *ptr & 0xfc ) == 0xf8)
            {
                l = 3;
            }
            else if (( *ptr & 0xfe ) == 0xfc)
            {
                l = 4;
            }
        }
        else
        {
            l--;
        }
    }
    while (*ptr++);

    if (l)
    {
        return NULL;
    }

    if (remote)
    {
        // target: user inbox - record username/email and return NULL
        if (( c.size() == 2 ) && !c[1].size())
        {
            if (user)
            {
                *user = c[0];
            }

            return NULL;
        }

        MegaUserList * usersList = api->getContacts();
        MegaUser *u = NULL;
        for (int i = 0; i < usersList->size(); i++)
        {
            if (usersList->get(i)->getEmail() == c[0])
            {
                u = usersList->get(i);
                break;
            }
        }

        if (u)
        {
            MegaNodeList* inshares = api->getInShares(u);
            for (int i = 0; i < inshares->size(); i++)
            {
                if (inshares->get(i)->getName() == c[1])
                {
                    n = inshares->get(i)->copy();
                    l = 2;
                    break;
                }
            }

            delete inshares;
        }
        delete usersList;

        if (!l)
        {
            return NULL;
        }
    }
    else //local
    {
        // path starting with /
        if (( c.size() > 1 ) && !c[0].size())
        {
            // path starting with //
            if (( c.size() > 2 ) && !c[1].size())
            {
                if (c[2] == "in")
                {
                    n = api->getInboxNode();
                }
                else if (c[2] == "bin")
                {
                    n = api->getRubbishNode();
                }
                else
                {
                    return NULL;
                }

                l = 3;
            }
            else
            {
                n = api->getRootNode();
                l = 1;
            }
        }
        else
        {
            n = api->getNodeByHandle(cwd);
        }
    }

    // parse relative path
    while (n && l < (int)c.size())
    {
        if (c[l] != ".")
        {
            if (c[l] == "..")
            {
                MegaNode * aux;
                aux = n;
                n = api->getParentNode(n);
                if (n != aux)
                {
                    delete aux;
                }
            }
            else
            {
                // locate child node (explicit ambiguity resolution: not implemented)
                if (c[l].size())
                {
                    bool isversion = nodeNameIsVersion(c[l]);
                    if (isversion)
                    {
                        MegaNode *baseNode = api->getChildNode(n, c[l].substr(0,c[l].size()-11).c_str());
                        if (baseNode)
                        {
                            MegaNodeList *versionNodes = api->getVersions(baseNode);
                            if (versionNodes)
                            {
                                for (int i = 0; i < versionNodes->size(); i++)
                                {
                                    MegaNode *versionNode = versionNodes->get(i);
                                    if ( c[l].substr(c[l].size()-10) == SSTR(versionNode->getModificationTime()) )
                                    {
                                        nn = versionNode->copy();
                                        break;
                                    }
                                }
                                delete versionNodes;
                            }
                            delete baseNode;
                        }
                    }
                    else
                    {
                        nn = api->getChildNode(n, c[l].c_str());
                    }

                    if (!nn) //NOT FOUND
                    {
                        // mv command target? return name part of not found
                        if (namepart && ( l == (int)c.size() - 1 )) //if this is the last part, we will pass that one, so that a mv command know the name to give the new node
                        {
                            *namepart = c[l];
                            return n;
                        }

                        delete n;
                        return NULL;
                    }

                    if (n != nn)
                    {
                        delete n;
                    }
                    n = nn;
                }
            }
        }

        l++;
    }

    return n;
}

/**
 * @brief MegaCmdExecuter::getPathsMatching Gets paths of nodes matching a pattern given its path parts and a parent node
 *
 * @param parentNode node for reference for relative paths
 * @param pathParts path pattern (separated in strings)
 * @param pathsMatching for the returned paths
 * @param usepcre use PCRE expressions if available
 * @param pathPrefix prefix to append to paths
 */
void MegaCmdExecuter::getPathsMatching(MegaNode *parentNode, deque<string> pathParts, vector<string> *pathsMatching, bool usepcre, string pathPrefix)
{
    if (!pathParts.size())
    {
        return;
    }

    string currentPart = pathParts.front();
    pathParts.pop_front();

    if (currentPart == "." || currentPart == "")
    {
        if (pathParts.size() == 0  /*&& currentPart == "."*/) //last leave.  // for consistency we also take parent when ended in / even if it's not a folder
         {
             pathsMatching->push_back(pathPrefix+currentPart);
         }

        //ignore this part
        return getPathsMatching(parentNode, pathParts, pathsMatching, usepcre, pathPrefix+"./");
    }
    if (currentPart == "..")
    {
        if (parentNode->getParentHandle())
        {
            if (!pathParts.size())
            {
                pathsMatching->push_back(pathPrefix+"..");
            }

            parentNode = api->getNodeByHandle(parentNode->getParentHandle());
            return getPathsMatching(parentNode, pathParts, pathsMatching, usepcre, pathPrefix+"../");
            delete parentNode;
        }
        else
        {
            return; //trying to access beyond root node
        }
    }

    MegaNodeList* children = api->getChildren(parentNode);
    if (children)
    {
        bool isversion = nodeNameIsVersion(currentPart);

        for (int i = 0; i < children->size(); i++)
        {
            MegaNode *childNode = children->get(i);
            // get childname from its path: alternative: childNode->getName()
            char *childNodePath = api->getNodePath(childNode);
            char *aux;
            aux = childNodePath+strlen(childNodePath);
            while (aux>childNodePath){
                if (*aux=='/' && *(aux-1) != '\\')  break;
                aux--;
            }
            if (*aux=='/') aux++;
            string childname(aux);
            delete []childNodePath;

            if (isversion)
            {

                if (childNode && patternMatches(childname.c_str(), currentPart.substr(0,currentPart.size()-11).c_str(), usepcre))
                {
                    MegaNodeList *versionNodes = api->getVersions(childNode);
                    if (versionNodes)
                    {
                        for (int i = 0; i < versionNodes->size(); i++)
                        {
                            MegaNode *versionNode = versionNodes->get(i);
                            if ( currentPart.substr(currentPart.size()-10) == SSTR(versionNode->getModificationTime()) )
                            {
                                if (pathParts.size() == 0) //last leave
                                {
                                    pathsMatching->push_back(pathPrefix+childname+"#"+SSTR(versionNode->getModificationTime())); //TODO: def version separator elswhere
                                }
                                else
                                {
                                    getPathsMatching(versionNode, pathParts, pathsMatching, usepcre,pathPrefix+childname+"#"+SSTR(versionNode->getModificationTime())+"/");
                                }

                                break;
                            }
                        }
                        delete versionNodes;
                    }
                }
            }
            else
            {
                if (patternMatches(childname.c_str(), currentPart.c_str(), usepcre))
                {
                    if (pathParts.size() == 0) //last leave
                    {
                        pathsMatching->push_back(pathPrefix+childname);
                    }
                    else
                    {
                        getPathsMatching(childNode, pathParts, pathsMatching, usepcre,pathPrefix+childname+"/");
                    }
                }


            }

        }

        delete children;
    }
}

/**
 * @brief MegaCmdExecuter::nodesPathsbypath returns paths of nodes that match a determined by path pattern
 * path naming conventions:
 * path is relative to cwd
 * /path is relative to ROOT
 * //in is in INBOX
 * //bin is in RUBBISH
 * X: is user X's INBOX
 * X:SHARE is share SHARE from user X
 * : and / filename components, as well as the \, must be escaped by \.
 * (correct UTF-8 encoding is assumed)
 *
 * You take the ownership of the returned value
 * @param ptr
 * @param usepcre use PCRE expressions if available
 * @param user
 * @param namepart
 * @return
 */
vector <string> * MegaCmdExecuter::nodesPathsbypath(const char* ptr, bool usepcre, string* user, string* namepart)
{
    vector<string> *pathsMatching = new vector<string> ();
    deque<string> c;
    string s;
    int l = 0;
    const char* bptr = ptr;
    int remote = 0; //shared
    MegaNode* n = NULL;
    bool isrelative = false;

    if (*ptr == '\0')
    {
        LOG_warn << "Trying to get node Paths for a node whose path is \"\"";
        return pathsMatching;
    }

    // split path by / or :
    do
    {
        if (!l)
        {
            if (*(const signed char*)ptr >= 0)
            {
                if (*ptr == '\\')
                {
                    if (ptr > bptr)
                    {
                        s.append(bptr, ptr - bptr);
                    }

                    bptr = ++ptr;

                    if (*bptr == 0)
                    {
                        c.push_back(s);
                        break;
                    }

                    ptr++;
                    continue;
                }

                if (( *ptr == '/' ) || ( *ptr == ':' ) || !*ptr)
                {
                    if (*ptr == ':')
                    {
                        if (c.size())
                        {
                            return pathsMatching;
                        }

                        remote = 1;
                    }

                    if (ptr > bptr)
                    {
                        s.append(bptr, ptr - bptr);
                    }

                    bptr = ptr + 1;

                    c.push_back(s);

                    s.erase();
                }
            }
            else if (( *ptr & 0xf0 ) == 0xe0)
            {
                l = 1;
            }
            else if (( *ptr & 0xf8 ) == 0xf0)
            {
                l = 2;
            }
            else if (( *ptr & 0xfc ) == 0xf8)
            {
                l = 3;
            }
            else if (( *ptr & 0xfe ) == 0xfc)
            {
                l = 4;
            }
        }
        else
        {
            l--;
        }
    }
    while (*ptr++);

    if (l)
    {
        delete pathsMatching;
        return NULL;
    }

    if (remote)
    {
        // target: user inbox - record username/email and return NULL
        if (( c.size() == 2 ) && !c.back().size())
        {
            if (user)
            {
                *user = c.front();
            }
            delete pathsMatching;
            return NULL;
        }

        MegaUserList * usersList = api->getContacts();
        MegaUser *u = NULL;
        for (int i = 0; i < usersList->size(); i++)
        {
            if (usersList->get(i)->getEmail() == c.front())
            {
                u = usersList->get(i);
                c.pop_front();
                break;
            }
        }

        if (u)
        {
            MegaNodeList* inshares = api->getInShares(u);
            for (int i = 0; i < inshares->size(); i++)
            {
                if (inshares->get(i)->getName() == c.front())
                {
                    n = inshares->get(i)->copy();
                    c.pop_front();
                    break;
                }
            }

            delete inshares;
        }
        delete usersList;
    }
    else // mine
    {

        // path starting with /
        if (( c.size() > 1 ) && !c.front().size())
        {
            c.pop_front();
            // path starting with //
            if (( c.size() > 1 ) && !c.front().size())
            {
                c.pop_front();
                if (c.front() == "in")
                {
                    n = api->getInboxNode();
                    c.pop_front();
                }
                else if (c.front() == "bin")
                {
                    n = api->getRubbishNode();
                    c.pop_front();
                }
                else
                {
                    if (c.size()==1) //last leave
                    {
                        string currentPart = c.front();
                        if (patternMatches("bin", currentPart.c_str(), usepcre))
                        {
                            pathsMatching->push_back("//bin");
                        }
                        if (patternMatches("in", currentPart.c_str(), usepcre))
                        {
                            pathsMatching->push_back("//in");
                        }
                        //shares?
                    }
                    return pathsMatching;
                }
            }
            else
            {
                n = api->getRootNode();
            }
        }
        else
        {
            n = api->getNodeByHandle(cwd);
            isrelative=true;
        }
    }

    string pathPrefix;
    if ((n) && !isrelative) //is root and not relative
    {
        char * nodepath = api->getNodePath(n);
        pathPrefix=nodepath;
        if (pathPrefix.size() && pathPrefix.at(pathPrefix.size()-1)!='/')
            pathPrefix+="/";
        delete []nodepath;
    }
    if (n)
    {
        while (c.size())
        {
            if (!c.back().size())
            {
                c.pop_back();
            }
            else
            {
                break;
            }
        }
        getPathsMatching(n, c, pathsMatching, usepcre, pathPrefix);
        delete n;
    }

    return pathsMatching;
}

/**
 *  You take the ownership of the nodes added in nodesMatching
 * @brief getNodesMatching
 * @param parentNode
 * @param c
 * @param nodesMatching
 */
void MegaCmdExecuter::getNodesMatching(MegaNode *parentNode, queue<string> pathParts, vector<MegaNode *> *nodesMatching, bool usepcre)
{
    if (!pathParts.size())
    {
        return;
    }

    string currentPart = pathParts.front();
    pathParts.pop();

    if (currentPart == "." || currentPart == "")
    {
        if (pathParts.size() == 0  /*&& currentPart == "."*/) //last leave.  // for consistency we also take parent when ended in / even if it's not a folder
        {
            if (parentNode)
            {
                nodesMatching->push_back(parentNode->copy());
                return;
            }
        }
        else
        {
            //ignore this part
            return getNodesMatching(parentNode, pathParts, nodesMatching, usepcre);
        }
    }
    if (currentPart == "..")
    {
        if (parentNode->getParentHandle())
        {
            MegaNode *newparentNode = api->getNodeByHandle(parentNode->getParentHandle());
            if (!pathParts.size()) //last leave
            {
                if (newparentNode)
                {
                    nodesMatching->push_back(newparentNode);
                }
                return;
            }
            else
            {
                getNodesMatching(newparentNode, pathParts, nodesMatching, usepcre);
                delete newparentNode;
                return;
            }

        }
        else
        {
            return; //trying to access beyond root node
        }

    }

    MegaNodeList* children = api->getChildren(parentNode);
    if (children)
    {
        bool isversion = nodeNameIsVersion(currentPart);

        for (int i = 0; i < children->size(); i++)
        {
            MegaNode *childNode = children->get(i);
            if (isversion)
            {

                if (childNode && patternMatches(childNode->getName(), currentPart.substr(0,currentPart.size()-11).c_str(), usepcre))
                {
                    MegaNodeList *versionNodes = api->getVersions(childNode);
                    if (versionNodes)
                    {
                        for (int i = 0; i < versionNodes->size(); i++)
                        {
                            MegaNode *versionNode = versionNodes->get(i);
                            if ( currentPart.substr(currentPart.size()-10) == SSTR(versionNode->getModificationTime()) )
                            {
                                if (pathParts.size() == 0) //last leave
                                {
                                    nodesMatching->push_back(versionNode->copy());
                                }
                                else
                                {
                                    getNodesMatching(versionNode, pathParts, nodesMatching, usepcre);
                                }

                                break;
                            }
                        }
                        delete versionNodes;
                    }
                }
            }
            else
            {

                if (patternMatches(childNode->getName(), currentPart.c_str(), usepcre))
                {
                    if (pathParts.size() == 0) //last leave
                    {
                        nodesMatching->push_back(childNode->copy());
                    }
                    else
                    {
                        getNodesMatching(childNode, pathParts, nodesMatching, usepcre);
                    }

                }
            }
        }

        delete children;
    }
}

MegaNode * MegaCmdExecuter::getRootNodeByPath(const char *ptr, string* user)
{
    queue<string> c;
    string s;
    int l = 0;
    const char* bptr = ptr;
    int remote = 0;
    MegaNode* n = NULL;

    // split path by / or :
    do
    {
        if (!l)
        {
            if (*(const signed char*)ptr >= 0)
            {
                if (*ptr == '\\')
                {
                    if (ptr > bptr)
                    {
                        s.append(bptr, ptr - bptr);
                    }

                    bptr = ++ptr;

                    if (*bptr == 0)
                    {
                        c.push(s);
                        break;
                    }

                    ptr++;
                    continue;
                }

                if (( *ptr == '/' ) || ( *ptr == ':' ) || !*ptr)
                {
                    if (*ptr == ':')
                    {
                        if (c.size())
                        {
                            return NULL;
                        }

                        remote = 1;
                    }

                    if (ptr > bptr)
                    {
                        s.append(bptr, ptr - bptr);
                    }

                    bptr = ptr + 1;

                    c.push(s);

                    s.erase();
                }
            }
            else if (( *ptr & 0xf0 ) == 0xe0)
            {
                l = 1;
            }
            else if (( *ptr & 0xf8 ) == 0xf0)
            {
                l = 2;
            }
            else if (( *ptr & 0xfc ) == 0xf8)
            {
                l = 3;
            }
            else if (( *ptr & 0xfe ) == 0xfc)
            {
                l = 4;
            }
        }
        else
        {
            l--;
        }
    }
    while (*ptr++);

    if (l)
    {
        return NULL;
    }

    if (remote)
    {
        // target: user inbox - record username/email and return NULL
        if (( c.size() == 2 ) && !c.back().size())
        {
            if (user)
            {
                *user = c.front();
            }

            return NULL;
        }
        MegaUserList * usersList = api->getContacts();
        MegaUser *u = NULL;
        for (int i = 0; i < usersList->size(); i++)
        {
            if (usersList->get(i)->getEmail() == c.front())
            {
                u = usersList->get(i);
                c.pop();
                break;
            }
        }

        if (u)
        {
            MegaNodeList* inshares = api->getInShares(u);
            for (int i = 0; i < inshares->size(); i++)
            {
                if (inshares->get(i)->getName() == c.front())
                {
                    n = inshares->get(i)->copy();
                    c.pop();
                    break;
                }
            }

            delete inshares;
        }
        delete usersList;
    }
    else //local
    {
        // path starting with /
        if (( c.size() > 1 ) && !c.front().size())
        {
            c.pop();
            // path starting with //
            if (( c.size() > 1 ) && !c.front().size())
            {
                c.pop();
                if (c.front() == "in")
                {
                    n = api->getInboxNode();
                    c.pop();
                }
                else if (c.front() == "bin")
                {
                    n = api->getRubbishNode();
                    c.pop();
                }
                else
                {
                    return NULL;
                }
            }
            else
            {
                n = api->getRootNode();
            }
        }
        else
        {
            n = api->getNodeByHandle(cwd);
        }
    }

    return n;
}

/**
 * @brief MegaCmdExecuter::nodesbypath
 * returns nodes determined by path pattern
 * path naming conventions:
 * path is relative to cwd
 * /path is relative to ROOT
 * //in is in INBOX
 * //bin is in RUBBISH
 * X: is user X's INBOX
 * X:SHARE is share SHARE from user X
 * : and / filename components, as well as the \, must be escaped by \.
 * (correct UTF-8 encoding is assumed)
 * @param ptr
 * @param usepcre use PCRE expressions if available
 * @param user
 * @return List of MegaNode*.  You take the ownership of those MegaNode*
 */
vector <MegaNode*> * MegaCmdExecuter::nodesbypath(const char* ptr, bool usepcre, string* user)
{
    vector<MegaNode *> *nodesMatching = new vector<MegaNode *> ();
    queue<string> c;
    string s;
    int l = 0;
    const char* bptr = ptr;
    int remote = 0; //shared
    MegaNode* n = NULL;

    if (*ptr == '\0')
    {
        LOG_warn << "Trying to get node whose path is \"\"";
        return nodesMatching;
    }

    // split path by / or :
    do
    {
        if (!l)
        {
            if (*(const signed char*)ptr >= 0)
            {
                if (*ptr == '\\')
                {
                    if (ptr > bptr)
                    {
                        s.append(bptr, ptr - bptr);
                    }

                    bptr = ++ptr;

                    if (*bptr == 0)
                    {
                        c.push(s);
                        break;
                    }

                    ptr++;
                    continue;
                }

                if (( *ptr == '/' ) || ( *ptr == ':' ) || !*ptr)
                {
                    if (*ptr == ':')
                    {
                        if (c.size())
                        {
                            return nodesMatching;
                        }

                        remote = 1;
                    }

                    if (ptr > bptr)
                    {
                        s.append(bptr, ptr - bptr);
                    }

                    bptr = ptr + 1;

                    c.push(s);

                    s.erase();
                }
            }
            else if (( *ptr & 0xf0 ) == 0xe0)
            {
                l = 1;
            }
            else if (( *ptr & 0xf8 ) == 0xf0)
            {
                l = 2;
            }
            else if (( *ptr & 0xfc ) == 0xf8)
            {
                l = 3;
            }
            else if (( *ptr & 0xfe ) == 0xfc)
            {
                l = 4;
            }
        }
        else
        {
            l--;
        }
    }
    while (*ptr++);

    if (l)
    {
        delete nodesMatching;
        return NULL;
    }

    if (remote)
    {
        // target: user inbox - record username/email and return NULL
        if (( c.size() == 2 ) && !c.back().size())
        {
            if (user)
            {
                *user = c.front();
            }
            delete nodesMatching;
            return NULL;
        }

        MegaUserList * usersList = api->getContacts();
        MegaUser *u = NULL;
        for (int i = 0; i < usersList->size(); i++)
        {
            if (usersList->get(i)->getEmail() == c.front())
            {
                u = usersList->get(i);
                c.pop();
                break;
            }
        }

        if (u)
        {
            MegaNodeList* inshares = api->getInShares(u);
            for (int i = 0; i < inshares->size(); i++)
            {
                if (inshares->get(i)->getName() == c.front())
                {
                    n = inshares->get(i)->copy();
                    c.pop();
                    break;
                }
            }

            delete inshares;
        }
        delete usersList;
    }
    else // mine
    {
        // path starting with /
        if (( c.size() > 1 ) && !c.front().size())
        {
            c.pop();
            // path starting with //
            if (( c.size() > 1 ) && !c.front().size())
            {
                c.pop();
                if (c.front() == "in")
                {
                    n = api->getInboxNode();
                    c.pop();
                }
                else if (c.front() == "bin")
                {
                    n = api->getRubbishNode();
                    c.pop();
                }
                else
                {
                    return nodesMatching;
                }
            }
            else
            {
                n = api->getRootNode();
            }
        }
        else
        {
            n = api->getNodeByHandle(cwd);
        }
    }
    if (n)
    {
        getNodesMatching(n, c, nodesMatching, usepcre);
        delete n;
    }

    return nodesMatching;
}

void MegaCmdExecuter::dumpNode(MegaNode* n, int extended_info, bool showversions, int depth, const char* title)
{
    if (!title && !( title = n->getName()))
    {
        title = "CRYPTO_ERROR";
    }

    if (depth)
    {
        for (int i = depth - 1; i--; )
        {
            OUTSTREAM << "\t";
        }
    }

    OUTSTREAM << title;
    if (extended_info)
    {
        //OUTSTREAM << "<" << api->handleToBase64(n->getHandle()) << ">";
        OUTSTREAM << " (";
        switch (n->getType())
        {
            case MegaNode::TYPE_FILE:
                OUTSTREAM << sizeToText(n->getSize(), false);

                const char* p;
                if (( p = strchr(n->getAttrString()->c_str(), ':')))
                {
                    OUTSTREAM << ", has attributes " << p + 1;
                }

                if (INVALID_HANDLE != n->getPublicHandle())
//            if (n->isExported())
                {
                    OUTSTREAM << ", shared as exported";
                    if (n->getExpirationTime())
                    {
                        OUTSTREAM << " temporal";
                    }
                    else
                    {
                        OUTSTREAM << " permanent";
                    }
                    OUTSTREAM << " file link";
                    if (extended_info > 1)
                    {
                        char * publicLink = n->getPublicLink();
                        OUTSTREAM << ": " << publicLink;
                        if (n->getExpirationTime())
                        {
                            if (n->isExpired())
                            {
                                OUTSTREAM << " expired at ";
                            }
                            else
                            {
                                OUTSTREAM << " expires at ";
                            }
                            OUTSTREAM << " at " << getReadableTime(n->getExpirationTime());
                        }
                        delete []publicLink;
                    }
                }
                break;

            case MegaNode::TYPE_FOLDER:
            {
                OUTSTREAM << "folder";
                MegaShareList* outShares = api->getOutShares(n);
                if (outShares)
                {
                    for (int i = 0; i < outShares->size(); i++)
                    {
                        if (outShares->get(i)->getNodeHandle() == n->getHandle())
                        {
                            OUTSTREAM << ", shared with " << outShares->get(i)->getUser() << ", access "
                                      << getAccessLevelStr(outShares->get(i)->getAccess());
                        }
                    }

                    MegaShareList* pendingoutShares = api->getPendingOutShares(n);
                    if (pendingoutShares)
                    {
                        for (int i = 0; i < pendingoutShares->size(); i++)
                        {
                            if (pendingoutShares->get(i)->getNodeHandle() == n->getHandle())
                            {
                                OUTSTREAM << ", shared (still pending)";
                                if (pendingoutShares->get(i)->getUser())
                                {
                                    OUTSTREAM << " with " << pendingoutShares->get(i)->getUser();
                                }
                                OUTSTREAM << " access " << getAccessLevelStr(pendingoutShares->get(i)->getAccess());
                            }
                        }

                        delete pendingoutShares;
                    }

                    if (UNDEF != n->getPublicHandle())
                    {
                        OUTSTREAM << ", shared as exported";
                        if (n->getExpirationTime())
                        {
                            OUTSTREAM << " temporal";
                        }
                        else
                        {
                            OUTSTREAM << " permanent";
                        }
                        OUTSTREAM << " folder link";
                        if (extended_info > 1)
                        {
                            char * publicLink = n->getPublicLink();
                            OUTSTREAM << ": " << publicLink;
                            delete []publicLink;
                        }
                    }
                    delete outShares;
                }

                if (n->isInShare())
                {
                    OUTSTREAM << ", inbound " << api->getAccess(n) << " share";
                }
                break;
            }
            case MegaNode::TYPE_ROOT:
            {
                OUTSTREAM << "root node";
                break;
            }
            case MegaNode::TYPE_INCOMING:
            {
                OUTSTREAM << "inbox";
                break;
            }
            case MegaNode::TYPE_RUBBISH:
            {
                OUTSTREAM << "rubbish";
                break;
            }
            default:
                OUTSTREAM << "unsupported type: " <<  n->getType() <<" , please upgrade";
        }
        OUTSTREAM << ")" << ( n->isRemoved() ? " (DELETED)" : "" );
    }

    OUTSTREAM << std::endl;

    if (showversions && n->getType() == MegaNode::TYPE_FILE)
    {
        MegaNodeList *versionNodes = api->getVersions(n);
        if (versionNodes)
        {
            for (int i = 0; i < versionNodes->size(); i++)
            {
                MegaNode *versionNode = versionNodes->get(i);

                if (versionNode->getHandle() != n->getHandle())
                {
                    string fullname(n->getName()?n->getName():"NO_NAME");
                    fullname += "#";
                    fullname += SSTR(versionNode->getModificationTime());
                    OUTSTREAM << "  " << fullname;
                    if (versionNode->getName() && !strcmp(versionNode->getName(),n->getName()) )
                    {
                        OUTSTREAM << "[" << (versionNode->getName()?versionNode->getName():"NO_NAME") << "]";
                    }
                    OUTSTREAM << " (" << getReadableTime(versionNode->getModificationTime()) << ")";
                    if (extended_info)
                    {
                        OUTSTREAM << " (" << sizeToText(versionNode->getSize(), false) << ")";
                    }
                    OUTSTREAM << std::endl;
                }
            }
        }
    }
}

void MegaCmdExecuter::dumpNodeSummaryHeader()
{
    OUTSTREAM << "FLAGS";
    OUTSTREAM << " ";
    OUTSTREAM << getFixLengthString("VERS", 4);
    OUTSTREAM << " ";
    OUTSTREAM << getFixLengthString("SIZE  ", 10 -1, ' ', true); //-1 because of "FLAGS"
    OUTSTREAM << " ";
    OUTSTREAM << getFixLengthString("DATE      ", 18, ' ', true);
    OUTSTREAM << " ";
    OUTSTREAM << "NAME";
    OUTSTREAM << std::endl;
}

void MegaCmdExecuter::dumpNodeSummary(MegaNode *n, bool humanreadable, const char *title)
{
    if (!title && !( title = n->getName()))
    {
        title = "CRYPTO_ERROR";
    }

    switch (n->getType())
    {
    case MegaNode::TYPE_FILE:
        OUTSTREAM << "-";
        break;
    case MegaNode::TYPE_FOLDER:
        OUTSTREAM << "d";
        break;
    case MegaNode::TYPE_ROOT:
        OUTSTREAM << "r";
        break;
    case MegaNode::TYPE_INCOMING:
        OUTSTREAM << "i";
        break;
    case MegaNode::TYPE_RUBBISH:
        OUTSTREAM << "b";
        break;
    default:
        OUTSTREAM << "x";
        break;
    }

    if (UNDEF != n->getPublicHandle())
    {
        OUTSTREAM << "e";
        if (n->getExpirationTime())
        {
            OUTSTREAM << "t";
        }
        else
        {
            OUTSTREAM << "p";
        }
    }
    else
    {
        OUTSTREAM << "--";
    }

    if (n->isShared())
    {
        OUTSTREAM << "s";
    }
    else if (n->isInShare())
    {
        OUTSTREAM << "i";
    }
    else
    {
        OUTSTREAM << "-";
    }

    OUTSTREAM << " ";

    if (n->isFile())
    {
        MegaNodeList *versionNodes = api->getVersions(n);
        int nversions = versionNodes ? versionNodes->size() : 0;
        if (nversions > 999)
        {
            OUTSTREAM << getFixLengthString(">999", 4, ' ', true);
        }
        else
        {
            OUTSTREAM << getFixLengthString(SSTR(nversions), 4, ' ', true);
        }

        delete versionNodes;
    }
    else
    {
        OUTSTREAM << getFixLengthString("-", 4, ' ', true);
    }

    OUTSTREAM << " ";

    if (n->isFile())
    {
        if (humanreadable)
        {
            OUTSTREAM << getFixLengthString(sizeToText(n->getSize()), 10, ' ', true);
        }
        else
        {
            OUTSTREAM << getFixLengthString(SSTR(n->getSize()), 10, ' ', true);
        }
    }
    else
    {
        OUTSTREAM << getFixLengthString("-", 10, ' ', true);
    }

    if (n->isFile())
    {
        OUTSTREAM << " " << getReadableShortTime(n->getModificationTime());
    }
    else
    {
        OUTSTREAM << " " << getReadableShortTime(n->getCreationTime());
    }


    OUTSTREAM << " " << title;
    OUTSTREAM << std::endl;
}



#ifdef ENABLE_BACKUPS

void MegaCmdExecuter::createOrModifyBackup(string local, string remote, string speriod, int numBackups)
{
    string locallocal;
    fsAccessCMD->path2local(&local, &locallocal);
    FileAccess *fa = fsAccessCMD->newfileaccess();
    if (!fa->isfolder(&locallocal))
    {
        setCurrentOutCode(MCMD_NOTFOUND);
        LOG_err << "Local path must be an existing folder: " << local;
        delete fa;
        return;
    }
    delete fa;


    int64_t period = -1;

    if (!speriod.size())
    {
        MegaBackup *backup = api->getBackupByPath(local.c_str());
        if (!backup)
        {
            backup = api->getBackupByTag(toInteger(local, -1));
        }
        if (backup)
        {
            speriod = backup->getPeriodString();
            if (!speriod.size())
            {
                period = backup->getPeriod();
            }
            delete backup;
        }
        else
        {
            setCurrentOutCode(MCMD_EARGS);
            LOG_err << "      " << getUsageStr("backup");
            return;
        }
    }
    if (speriod.find(" ") == string::npos && period == -1)
    {
        period = 10 * getTimeStampAfter(0,speriod);
        speriod = "";
    }

    if (numBackups == -1)
    {
        MegaBackup *backup = api->getBackupByPath(local.c_str());
        if (!backup)
        {
            backup = api->getBackupByTag(toInteger(local, -1));
        }
        if (backup)
        {
            numBackups = backup->getMaxBackups();
            delete backup;
        }
    }
    if (numBackups == -1)
    {
        setCurrentOutCode(MCMD_EARGS);
        LOG_err << "      " << getUsageStr("backup");
        return;
    }

    MegaNode *n = NULL;
    if (remote.size())
    {
        n = nodebypath(remote.c_str());
    }
    else
    {
        MegaBackup *backup = api->getBackupByPath(local.c_str());
        if (!backup)
        {
            backup = api->getBackupByTag(toInteger(local, -1));
        }
        if (backup)
        {
            n = api->getNodeByHandle(backup->getMegaHandle());
            delete backup;
        }
    }

    if (n)
    {
        if (n->getType() != MegaNode::TYPE_FOLDER)
        {
            setCurrentOutCode(MCMD_INVALIDTYPE);
            LOG_err << remote << " must be a valid folder";
        }
        else
        {
            if (establishBackup(local, n, period, speriod, numBackups) )
            {
                mtxBackupsMap.lock();
                ConfigurationManager::saveBackups(&ConfigurationManager::configuredBackups);
                mtxBackupsMap.unlock();
                OUTSTREAM << "Backup established: " << local << " into " << remote << " period="
                          << ((period != -1)?getReadablePeriod(period/10):"\""+speriod+"\"")
                          << " Number-of-Backups=" << numBackups << std::endl;
            }
        }
        delete n;
    }
    else
    {
        setCurrentOutCode(MCMD_NOTFOUND);
        LOG_err << remote << " not found";
    }
}
#endif

void MegaCmdExecuter::dumptree(MegaNode* n, int recurse, int extended_info, bool showversions, int depth, string pathRelativeTo)
{
    if (depth || ( n->getType() == MegaNode::TYPE_FILE ))
    {
        if (pathRelativeTo != "NULL")
        {
            if (!n->getName())
            {
                dumpNode(n, extended_info, showversions, depth, "CRYPTO_ERROR");
            }
            else
            {
                char * nodepath = api->getNodePath(n);

                char *pathToShow = NULL;
                if (pathRelativeTo != "")
                {
                    pathToShow = strstr(nodepath, pathRelativeTo.c_str());
                }

                if (pathToShow == nodepath)     //found at beginning
                {
                    pathToShow += pathRelativeTo.size();
                    if (( *pathToShow == '/' ) && ( pathRelativeTo != "/" ))
                    {
                        pathToShow++;
                    }
                }
                else
                {
                    pathToShow = nodepath;
                }

                dumpNode(n, extended_info, showversions, depth, pathToShow);

                delete []nodepath;
            }
        }
        else
        {
                dumpNode(n, extended_info, showversions, depth);
        }

        if (!recurse && depth)
        {
            return;
        }
    }

    if (n->getType() != MegaNode::TYPE_FILE)
    {
        MegaNodeList* children = api->getChildren(n);
        if (children)
        {
            for (int i = 0; i < children->size(); i++)
            {
                dumptree(children->get(i), recurse, extended_info, showversions, depth + 1);
            }

            delete children;
        }
    }
}

void MegaCmdExecuter::dumpTreeSummary(MegaNode *n, int recurse, bool show_versions, int depth, bool humanreadable, string pathRelativeTo)
{
    char * nodepath = api->getNodePath(n);

    string scryptoerror = "CRYPTO_ERROR";

    char *pathToShow = NULL;
    if (pathRelativeTo != "")
    {
        pathToShow = strstr(nodepath, pathRelativeTo.c_str());
    }

    if (pathToShow == nodepath) //found at beginning
    {
        pathToShow += pathRelativeTo.size();
        if (( *pathToShow == '/' ) && ( pathRelativeTo != "/" ))
        {
            pathToShow++;
        }
    }
    else
    {
        pathToShow = nodepath;
    }

    if (!pathToShow && !( pathToShow = (char *)n->getName()))
    {
        pathToShow = (char *)scryptoerror.c_str();
    }

    if (n->getType() != MegaNode::TYPE_FILE)
    {
        MegaNodeList* children = api->getChildren(n);
        if (children)
        {
            if (depth)
            {
                OUTSTREAM << std::endl;
            }

            if (recurse)
            {
                OUTSTREAM << pathToShow << ":" << std::endl;
            }

            for (int i = 0; i < children->size(); i++)
            {
                dumpNodeSummary(children->get(i), humanreadable);
            }

            if (show_versions)
            {
                for (int i = 0; i < children->size(); i++)
                {
                    MegaNode *c = children->get(i);

                    MegaNodeList *vers = api->getVersions(c);
                    if (vers &&  vers->size() > 1)
                    {
                        OUTSTREAM << std::endl << "Versions of " << pathToShow << "/" << c->getName() << ":" << std::endl;

                        for (int i = 0; i < vers->size(); i++)
                        {
                            dumpNodeSummary(vers->get(i), humanreadable);
                        }
                    }
                    delete vers;
                }
            }

            if (recurse)
            {
                for (int i = 0; i < children->size(); i++)
                {
                    MegaNode *c = children->get(i);
                    dumpTreeSummary(c, recurse, show_versions, depth + 1, humanreadable);
                }
            }
            delete children;
        }
    }
    else // file
    {
        if (!depth)
        {

            dumpNodeSummary(n, humanreadable);

            if (show_versions)
            {
                MegaNodeList *vers = api->getVersions(n);
                if (vers &&  vers->size() > 1)
                {
                    OUTSTREAM << std::endl << "Versions of " << pathToShow << ":" << std::endl;

                    for (int i = 0; i < vers->size(); i++)
                    {
                        string nametoshow = n->getName()+string("#")+SSTR(vers->get(i)->getModificationTime());
                        dumpNodeSummary(vers->get(i), humanreadable, nametoshow.c_str() );
                    }
                }
                delete vers;
            }
        }

    }
    delete []nodepath;
}


/**
 * @brief Tests if a path can be created
 * @param path
 * @return
 */
bool MegaCmdExecuter::TestCanWriteOnContainingFolder(string *path)
{
#ifdef _WIN32
    replaceAll(*path,"/","\\");
#endif
    string localpath;
    fsAccessCMD->path2local(path, &localpath);
    size_t lastpart = fsAccessCMD->lastpartlocal(&localpath);
    string containingFolder = ".";
    if (lastpart)
    {
        string firstpartlocal(localpath, 0, lastpart - fsAccessCMD->localseparator.size());
        fsAccessCMD->local2path(&firstpartlocal, &containingFolder);
    }

    string localcontainingFolder;
    fsAccessCMD->path2local(&containingFolder, &localcontainingFolder);
    FileAccess *fa = fsAccessCMD->newfileaccess();
    if (!fa->isfolder(&localcontainingFolder))
    {
        delete fa;
        setCurrentOutCode(MCMD_INVALIDTYPE);
        LOG_err << containingFolder << " is not a valid Download Folder";
        return false;
    }
    delete fa;
    if (!canWrite(containingFolder))
    {
        setCurrentOutCode(MCMD_NOTPERMITTED);
        LOG_err << "Write not allowed in " << containingFolder;
        return false;
    }
    return true;
}

MegaContactRequest * MegaCmdExecuter::getPcrByContact(string contactEmail)
{
    MegaContactRequestList *icrl = api->getIncomingContactRequests();
    if (icrl)
    {
        for (int i = 0; i < icrl->size(); i++)
        {
            if (icrl->get(i)->getSourceEmail() == contactEmail)
            {
                return icrl->get(i);

                delete icrl;
            }
        }

        delete icrl;
    }
    return NULL;
}

string MegaCmdExecuter::getDisplayPath(string givenPath, MegaNode* n)
{
    char * pathToNode = api->getNodePath(n);
    char * pathToShow = pathToNode;

    string pathRelativeTo = "NULL";
    string cwpath = getCurrentPath();
    string toret="";


    if (givenPath.find('/') == 0 )
    {
        pathRelativeTo = "";
    }
    else if(givenPath.find("../") == 0 || givenPath.find("./") == 0 )
    {
        pathRelativeTo = "";
        MegaNode *n = api->getNodeByHandle(cwd);
        while(true)
        {
            if(givenPath.find("./") == 0)
            {
                givenPath=givenPath.substr(2);
                toret+="./";
                if (n)
                {
                    char *npath = api->getNodePath(n);
                    pathRelativeTo = string(npath);
                    delete []npath;
                }
                return toret;

            }
            else if(givenPath.find("../") == 0)
            {
                givenPath=givenPath.substr(3);
                toret+="../";
                MegaNode *aux = n;
                if (n)
                {
                    n=api->getNodeByHandle(n->getParentHandle());
                }
                delete aux;
                if (n)
                {
                    char *npath = api->getNodePath(n);
                    pathRelativeTo = string(npath);
                    delete []npath;
                }
            }
            else
            {
                break;
            }
        }
        delete n;
    }
    else
    {
        if (cwpath == "/") //TODO: //bin /X:share ...
        {
            pathRelativeTo = cwpath;
        }
        else
        {
            pathRelativeTo = cwpath + "/";
        }
    }

    if (( "" == givenPath ) && !strcmp(pathToNode, cwpath.c_str()))
    {
        assert(strlen(pathToNode)>0);
        pathToNode[0] = '.';
        pathToNode[1] = '\0';
    }

    if (pathRelativeTo != "")
    {
        pathToShow = strstr(pathToNode, pathRelativeTo.c_str());
    }

    if (pathToShow == pathToNode)     //found at beginning
    {
        if (strcmp(pathToNode, "/"))
        {
            pathToShow += pathRelativeTo.size();
        }
    }
    else
    {
        pathToShow = pathToNode;
    }

    toret+=pathToShow;
    delete []pathToNode;
    return toret;
}

int MegaCmdExecuter::dumpListOfExported(MegaNode* n, string givenPath)
{
    int toret = 0;
    vector<MegaNode *> listOfExported;
    processTree(n, includeIfIsExported, (void*)&listOfExported);
    for (std::vector< MegaNode * >::iterator it = listOfExported.begin(); it != listOfExported.end(); ++it)
    {
        MegaNode * n = *it;
        if (n)
        {
            string pathToShow = getDisplayPath(givenPath, n);
            dumpNode(n, 2, 1, false, pathToShow.c_str());

            delete n;
        }
    }
    toret = int(listOfExported.size());
    listOfExported.clear();
    return toret;
}

/**
 * @brief listnodeshares For a node, it prints all the shares it has
 * @param n
 * @param name
 */
void MegaCmdExecuter::listnodeshares(MegaNode* n, string name)
{
    MegaShareList* outShares = api->getOutShares(n);
    if (outShares)
    {
        for (int i = 0; i < outShares->size(); i++)
        {
            OUTSTREAM << name ? name : n->getName();

            if (outShares->get(i))
            {
                OUTSTREAM << ", shared with " << outShares->get(i)->getUser() << " (" << getAccessLevelStr(outShares->get(i)->getAccess()) << ")"
                          << std::endl;
            }
            else
            {
                OUTSTREAM << ", shared as exported folder link" << std::endl;
            }
        }

        delete outShares;
    }
}

void MegaCmdExecuter::dumpListOfShared(MegaNode* n, string givenPath)
{
    vector<MegaNode *> listOfShared;
    processTree(n, includeIfIsShared, (void*)&listOfShared);
    if (!listOfShared.size())
    {
        setCurrentOutCode(MCMD_NOTFOUND);
        LOG_err << "No shared found for given path: " << givenPath;
    }
    for (std::vector< MegaNode * >::iterator it = listOfShared.begin(); it != listOfShared.end(); ++it)
    {
        MegaNode * n = *it;
        if (n)
        {
            string pathToShow = getDisplayPath(givenPath, n);
            //dumpNode(n, 3, 1,pathToShow.c_str());
            listnodeshares(n, pathToShow);

            delete n;
        }
    }

    listOfShared.clear();
}

//includes pending and normal shares
void MegaCmdExecuter::dumpListOfAllShared(MegaNode* n, string givenPath)
{
    vector<MegaNode *> listOfShared;
    processTree(n, includeIfIsSharedOrPendingOutShare, (void*)&listOfShared);
    for (std::vector< MegaNode * >::iterator it = listOfShared.begin(); it != listOfShared.end(); ++it)
    {
        MegaNode * n = *it;
        if (n)
        {
            string pathToShow = getDisplayPath(givenPath, n);
            dumpNode(n, 3, false, 1, pathToShow.c_str());
            //notice: some nodes may be dumped twice

            delete n;
        }
    }

    listOfShared.clear();
}

void MegaCmdExecuter::dumpListOfPendingShares(MegaNode* n, string givenPath)
{
    vector<MegaNode *> listOfShared;
    processTree(n, includeIfIsPendingOutShare, (void*)&listOfShared);

    for (std::vector< MegaNode * >::iterator it = listOfShared.begin(); it != listOfShared.end(); ++it)
    {
        MegaNode * n = *it;
        if (n)
        {
            string pathToShow = getDisplayPath(givenPath, n);
            dumpNode(n, 3, false, 1, pathToShow.c_str());

            delete n;
        }
    }

    listOfShared.clear();
}


void MegaCmdExecuter::loginWithPassword(char *password)
{
    MegaCmdListener *megaCmdListener = new MegaCmdListener(NULL);
    api->login(login.c_str(), password, megaCmdListener);
    actUponLogin(megaCmdListener);
    delete megaCmdListener;
}


void MegaCmdExecuter::changePassword(const char *oldpassword, const char *newpassword)
{
    MegaCmdListener *megaCmdListener = new MegaCmdListener(NULL);
    api->changePassword(oldpassword, newpassword, megaCmdListener);
    megaCmdListener->wait();
    if (!checkNoErrors(megaCmdListener->getError(), "change password"))
    {
        LOG_err << "Please, ensure you enter the old password correctly";
    }
    else
    {
        OUTSTREAM << "Password changed succesfully" << std::endl;
    }
    delete megaCmdListener;
}

void MegaCmdExecuter::actUponGetExtendedAccountDetails(SynchronousRequestListener *srl, int timeout)
{
    if (timeout == -1)
    {
        srl->wait();
    }
    else
    {
        int trywaitout = srl->trywait(timeout);
        if (trywaitout)
        {
            LOG_err << "GetExtendedAccountDetails took too long, it may have failed. No further actions performed";
            return;
        }
    }

    if (checkNoErrors(srl->getError(), "failed to GetExtendedAccountDetails"))
    {
        char timebuf[32], timebuf2[32];

        LOG_verbose << "actUponGetExtendedAccountDetails ok";

        MegaAccountDetails *details = srl->getRequest()->getMegaAccountDetails();
        if (details)
        {
            OUTSTREAM << "    Available storage:"
                      << getFixLengthString(sizeToText(details->getStorageMax()), 9, ' ', true)
                      << "ytes" << std::endl;
            MegaNode *n = api->getRootNode();
            if (n)
            {
                OUTSTREAM << "        In ROOT:      "
                          << getFixLengthString(sizeToText(details->getStorageUsed(n->getHandle())), 9, ' ', true) << "ytes in "
                          << getFixLengthString(SSTR(details->getNumFiles(n->getHandle())),5,' ',true) << " file(s) and "
                          << getFixLengthString(SSTR(details->getNumFolders(n->getHandle())),5,' ',true) << " folder(s)" << std::endl;
                delete n;
            }

            n = api->getInboxNode();
            if (n)
            {
                OUTSTREAM << "        In INBOX:     "
                          << getFixLengthString( sizeToText(details->getStorageUsed(n->getHandle())), 9, ' ', true ) << "ytes in "
                          << getFixLengthString(SSTR(details->getNumFiles(n->getHandle())),5,' ',true) << " file(s) and "
                          << getFixLengthString(SSTR(details->getNumFolders(n->getHandle())),5,' ',true) << " folder(s)" << std::endl;
                delete n;
            }

            n = api->getRubbishNode();
            if (n)
            {
                OUTSTREAM << "        In RUBBISH:   "
                          << getFixLengthString(sizeToText(details->getStorageUsed(n->getHandle())), 9, ' ', true) << "ytes in "
                          << getFixLengthString(SSTR(details->getNumFiles(n->getHandle())),5,' ',true) << " file(s) and "
                          << getFixLengthString(SSTR(details->getNumFolders(n->getHandle())),5,' ',true) << " folder(s)" << std::endl;
                delete n;
            }

            long long usedinVersions = details->getVersionStorageUsed();

            OUTSTREAM << "        Total size taken up by file versions: "
                      << getFixLengthString(sizeToText(usedinVersions), 12, ' ', true) << "ytes"<< std::endl;


            MegaNodeList *inshares = api->getInShares();
            if (inshares)
            {
                for (int i = 0; i < inshares->size(); i++)
                {
                    n = inshares->get(i);
                    OUTSTREAM << "        In INSHARE " << n->getName() << ": " << details->getStorageUsed(n->getHandle()) << " byte(s) in "
                              << details->getNumFiles(n->getHandle()) << " file(s) and " << details->getNumFolders(n->getHandle()) << " folder(s)" << std::endl;
                }
            }
            delete inshares;

            OUTSTREAM << "    Pro level: " << details->getProLevel() << std::endl;
            if (details->getProLevel())
            {
                if (details->getProExpiration())
                {
                    time_t ts = details->getProExpiration();
                    strftime(timebuf, sizeof timebuf, "%c", localtime(&ts));
                    OUTSTREAM << "        " << "Pro expiration date: " << timebuf << std::endl;
                }
            }
            char * subscriptionMethod = details->getSubscriptionMethod();
            OUTSTREAM << "    Subscription type: " << subscriptionMethod << std::endl;
            delete []subscriptionMethod;
            OUTSTREAM << "    Account balance:" << std::endl;
            for (int i = 0; i < details->getNumBalances(); i++)
            {
                MegaAccountBalance * balance = details->getBalance(i);
                char sbalance[50];
                sprintf(sbalance, "    Balance: %.3s %.02f", balance->getCurrency(), balance->getAmount());
                OUTSTREAM << "    " << "Balance: " << sbalance << std::endl;
            }

            if (details->getNumPurchases())
            {
                OUTSTREAM << "Purchase history:" << std::endl;
                for (int i = 0; i < details->getNumPurchases(); i++)
                {
                    MegaAccountPurchase *purchase = details->getPurchase(i);

                    time_t ts = purchase->getTimestamp();
                    char spurchase[150];

                    strftime(timebuf, sizeof timebuf, "%c", localtime(&ts));
                    sprintf(spurchase, "ID: %.11s Time: %s Amount: %.3s %.02f Payment method: %d\n",
                        purchase->getHandle(), timebuf, purchase->getCurrency(), purchase->getAmount(), purchase->getMethod());
                    OUTSTREAM << "    " << spurchase << std::endl;
                }
            }

            if (details->getNumTransactions())
            {
                OUTSTREAM << "Transaction history:" << std::endl;
                for (int i = 0; i < details->getNumTransactions(); i++)
                {
                    MegaAccountTransaction *transaction = details->getTransaction(i);
                    time_t ts = transaction->getTimestamp();
                    char stransaction[100];
                    strftime(timebuf, sizeof timebuf, "%c", localtime(&ts));
                    sprintf(stransaction, "ID: %.11s Time: %s Amount: %.3s %.02f\n",
                        transaction->getHandle(), timebuf, transaction->getCurrency(), transaction->getAmount());
                    OUTSTREAM << "    " << stransaction << std::endl;
                }
            }

            int alive_sessions = 0;
            OUTSTREAM << "Current Active Sessions:" << std::endl;
            char sdetails[500];
            for (int i = 0; i < details->getNumSessions(); i++)
            {
                MegaAccountSession * session = details->getSession(i);
                if (session->isAlive())
                {
                    sdetails[0]='\0';
                    time_t ts = session->getCreationTimestamp();
                    strftime(timebuf,  sizeof timebuf, "%c", localtime(&ts));
                    ts = session->getMostRecentUsage();
                    strftime(timebuf2, sizeof timebuf, "%c", localtime(&ts));

                    char *sid = api->userHandleToBase64(session->getHandle());

                    if (session->isCurrent())
                    {
                        sprintf(sdetails, "    * Current Session\n");
                    }

                    char * userAgent = session->getUserAgent();
                    char * country = session->getCountry();
                    char * ip = session->getIP();

                    sprintf(sdetails, "%s    Session ID: %s\n    Session start: %s\n    Most recent activity: %s\n    IP: %s\n    Country: %.2s\n    User-Agent: %s\n    -----\n",
                    sdetails,
                    sid,
                    timebuf,
                    timebuf2,
                    ip,
                    country,
                    userAgent
                    );
                    OUTSTREAM << sdetails;
                    delete []sid;
                    delete []userAgent;
                    delete []country;
                    delete []ip;
                    alive_sessions++;
                }
                delete session;
            }

            if (alive_sessions)
            {
                OUTSTREAM << alive_sessions << " active sessions opened" << std::endl;
            }
            delete details;
        }
    }
}

bool MegaCmdExecuter::actUponFetchNodes(MegaApi *api, SynchronousRequestListener *srl, int timeout)
{
    if (timeout == -1)
    {
        srl->wait();
    }
    else
    {
        int trywaitout = srl->trywait(timeout);
        if (trywaitout)
        {
            LOG_err << "Fetch nodes took too long, it may have failed. No further actions performed";
            return false;
        }
    }

    if (checkNoErrors(srl->getError(), "fetch nodes"))
    {
        LOG_verbose << "actUponFetchNodes ok";
        api->enableTransferResumption();

        MegaNode *cwdNode = ( cwd == UNDEF ) ? NULL : api->getNodeByHandle(cwd);
        if (( cwd == UNDEF ) || !cwdNode)
        {
            MegaNode *rootNode = srl->getApi()->getRootNode();
            cwd = rootNode->getHandle();
            delete rootNode;
        }
        if (cwdNode)
        {
            delete cwdNode;
        }
        updateprompt(api, cwd);
        LOG_debug << " Fetch nodes correctly";
        return true;
    }
    return false;
}

void MegaCmdExecuter::actUponLogin(SynchronousRequestListener *srl, int timeout)
{
    if (timeout == -1)
    {
        srl->wait();
    }
    else
    {
        int trywaitout = srl->trywait(timeout);
        if (trywaitout)
        {
            LOG_err << "Login took too long, it may have failed. No further actions performed";
            return;
        }
    }

    LOG_debug << "actUponLogin login";

    if (srl->getRequest()->getEmail())
    {
        LOG_debug << "actUponLogin login email: " << srl->getRequest()->getEmail();
    }

    if (srl->getError()->getErrorCode() == MegaError::API_ENOENT) // failed to login
    {
        LOG_err << "Login failed: invalid email or password";
    }
    else if (srl->getError()->getErrorCode() == MegaError::API_EINCOMPLETE)
    {
        LOG_err << "Login failed: unconfirmed account. Please confirm your account";
    }
    else if (checkNoErrors(srl->getError(), "Login")) //login success:
    {
        LOG_debug << "Login correct ... " << (srl->getRequest()->getEmail()?srl->getRequest()->getEmail():"");
        /* Restoring configured values */
        session = srl->getApi()->dumpSession();
        ConfigurationManager::saveSession(session);
        mtxSyncMap.lock();
        ConfigurationManager::loadsyncs();
        mtxSyncMap.unlock();
#ifdef ENABLE_BACKUPS
        mtxBackupsMap.lock();
        ConfigurationManager::loadbackups();
        mtxBackupsMap.unlock();
#endif

        ConfigurationManager::loadExcludedNames();
        ConfigurationManager::loadConfiguration(false);
        std::vector<string> vexcludednames(ConfigurationManager::excludedNames.begin(), ConfigurationManager::excludedNames.end());
        api->setExcludedNames(&vexcludednames);

        long long maxspeeddownload = ConfigurationManager::getConfigurationValue("maxspeeddownload", -1);
        if (maxspeeddownload != -1) api->setMaxDownloadSpeed(maxspeeddownload);
        long long maxspeedupload = ConfigurationManager::getConfigurationValue("maxspeedupload", -1);
        if (maxspeedupload != -1) api->setMaxUploadSpeed(maxspeedupload);

        api->useHttpsOnly(ConfigurationManager::getConfigurationValue("https", false));

#ifndef _WIN32
        string permissionsFiles = ConfigurationManager::getConfigurationSValue("permissionsFiles");
        if (permissionsFiles.size())
        {
            int perms = permissionsFromReadable(permissionsFiles);
            if (perms != -1)
            {
                api->setDefaultFilePermissions(perms);
            }
        }
        string permissionsFolders = ConfigurationManager::getConfigurationSValue("permissionsFolders");
        if (permissionsFolders.size())
        {
            int perms = permissionsFromReadable(permissionsFolders);
            if (perms != -1)
            {
                api->setDefaultFolderPermissions(perms);
            }
        }
#endif

        LOG_info << "Fetching nodes ... ";
        srl->getApi()->fetchNodes(srl);
        actUponFetchNodes(api, srl, timeout);
        MegaUser *u = api->getMyUser();
        if (u)
        {
            LOG_info << "Login complete as " << u->getEmail();
            delete u;
        }

#ifdef ENABLE_BACKUPS
        mtxBackupsMap.lock();
        if (ConfigurationManager::configuredBackups.size())
        {
            LOG_info << "Restablishing backups ... ";
            unsigned int i=0;
            for (map<string, backup_struct *>::iterator itr = ConfigurationManager::configuredBackups.begin();
                 itr != ConfigurationManager::configuredBackups.end(); ++itr, i++)
            {
                backup_struct *thebackup = itr->second;

                MegaNode * node = api->getNodeByHandle(thebackup->handle);
                if (establishBackup(thebackup->localpath, node, thebackup->period, thebackup->speriod, thebackup->numBackups))
                {
                    thebackup->failed = false;
                    const char *nodepath = api->getNodePath(node);
                    LOG_debug << "Succesfully resumed backup: " << thebackup->localpath << " to " << nodepath;
                    delete []nodepath;
                }
                else
                {
                    thebackup->failed = true;
                    char *nodepath = api->getNodePath(node);
                    LOG_err << "Failed to resume backup: " << thebackup->localpath << " to " << nodepath;
                    delete []nodepath;
                }

                delete node;
            }

            ConfigurationManager::saveBackups(&ConfigurationManager::configuredBackups);
        }
        mtxBackupsMap.unlock();
#endif

#ifdef HAVE_LIBUV
        // restart webdav
        int port = ConfigurationManager::getConfigurationValue("webdav_port", -1);
        if (port != -1)
        {
            bool localonly = ConfigurationManager::getConfigurationValue("webdav_localonly", -1);
            bool tls = ConfigurationManager::getConfigurationValue("webdav_tls", false);
            string pathtocert, pathtokey;
            pathtocert = ConfigurationManager::getConfigurationSValue("webdav_cert");
            pathtokey = ConfigurationManager::getConfigurationSValue("webdav_key");

            api->httpServerEnableFolderServer(true);
            if (api->httpServerStart(localonly, port, tls, pathtocert.c_str(), pathtokey.c_str()))
            {
                list<string> servedpaths = ConfigurationManager::getConfigurationValueList<string>("webdav_served_locations");

                for ( std::list<string>::iterator it = servedpaths.begin(); it != servedpaths.end(); ++it){
                    string pathToServe = *it;
                    if (pathToServe.size())
                    {
                        MegaNode *n = nodebypath(pathToServe.c_str());
                        if (n)
                        {
                            char *l = api->httpServerGetLocalWebDavLink(n);
                            LOG_debug << "Serving via webdav: " << pathToServe << ": " << l;
                            delete []l;
                            delete n;
                        }
                        else
                        {
                            LOG_warn << "Could no find location to server via webdav: " << pathToServe;
                        }
                    }
                }

                LOG_info << "Webdav server restored due to saved configuration";
            }
            else
            {
                LOG_err << "Failed to initialize WEBDAV server";
            }
        }
#endif
    }

#if defined(_WIN32) || defined(__APPLE__)

    MegaCmdListener *megaCmdListener = new MegaCmdListener(NULL);
    srl->getApi()->getLastAvailableVersion("BdARkQSQ",megaCmdListener);
    megaCmdListener->wait();

    if (!megaCmdListener->getError())
    {
        LOG_fatal << "No MegaError at getLastAvailableVersion: ";
    }
    else if (megaCmdListener->getError()->getErrorCode() != MegaError::API_OK)
    {
        LOG_debug << "Couldn't get latests available version: " << megaCmdListener->getError()->getErrorString();
    }
    else
    {
        if (megaCmdListener->getRequest()->getNumber() != MEGACMD_CODE_VERSION)
        {
            OUTSTREAM << "---------------------------------------------------------------------" << std::endl;
            OUTSTREAM << "--        There is a new version available of megacmd: " << std::setw(12) << std::left << megaCmdListener->getRequest()->getName() << "--" << std::endl;
            OUTSTREAM << "--        Please, download it from https://mega.nz/cmd             --" << std::endl;
            OUTSTREAM << "---------------------------------------------------------------------" << std::endl;
        }
    }
    delete megaCmdListener;
#endif

}

void MegaCmdExecuter::actUponLogout(SynchronousRequestListener *srl, bool keptSession, int timeout)
{
    if (!timeout)
    {
        srl->wait();
    }
    else
    {
        int trywaitout = srl->trywait(timeout);
        if (trywaitout)
        {
            LOG_err << "Logout took too long, it may have failed. No further actions performed";
            return;
        }
    }
    if (checkNoErrors(srl->getError(), "logout"))
    {
        LOG_verbose << "actUponLogout logout ok";
        cwd = UNDEF;
        delete []session;
        session = NULL;
        mtxSyncMap.lock();
        ConfigurationManager::unloadConfiguration();
        if (!keptSession)
        {
            ConfigurationManager::saveSession("");
            ConfigurationManager::saveBackups(&ConfigurationManager::configuredBackups);
            ConfigurationManager::saveSyncs(&ConfigurationManager::configuredSyncs);
        }
        ConfigurationManager::clearConfigurationFile();
        mtxSyncMap.unlock();
    }
    updateprompt(api, cwd);
}

int MegaCmdExecuter::actUponCreateFolder(SynchronousRequestListener *srl, int timeout)
{
    if (!timeout)
    {
        srl->wait();
    }
    else
    {
        int trywaitout = srl->trywait(timeout);
        if (trywaitout)
        {
            LOG_err << "actUponCreateFolder took too long, it may have failed. No further actions performed";
            return 1;
        }
    }
    if (checkNoErrors(srl->getError(), "create folder"))
    {
        LOG_verbose << "actUponCreateFolder Create Folder ok";
        return 0;
    }

    return 2;
}

void MegaCmdExecuter::confirmDelete()
{
    if (nodesToConfirmDelete.size())
    {
        MegaNode * nodeToConfirmDelete = nodesToConfirmDelete.front();
        nodesToConfirmDelete.erase(nodesToConfirmDelete.begin());
        doDeleteNode(nodeToConfirmDelete,api);
    }


    if (nodesToConfirmDelete.size())
    {
        string newprompt("Are you sure to delete ");
        newprompt+=nodesToConfirmDelete.front()->getName();
        newprompt+=" ? (Yes/No/All/None): ";
        setprompt(AREYOUSURETODELETE,newprompt);
    }
    else
    {
        setprompt(COMMAND);
    }

}

void MegaCmdExecuter::discardDelete()
{
    if (nodesToConfirmDelete.size()){
        delete nodesToConfirmDelete.front();
        nodesToConfirmDelete.erase(nodesToConfirmDelete.begin());
    }
    if (nodesToConfirmDelete.size())
    {
        string newprompt("Are you sure to delete ");
        newprompt+=nodesToConfirmDelete.front()->getName();
        newprompt+=" ? (Yes/No/All/None): ";
        setprompt(AREYOUSURETODELETE,newprompt);
    }
    else
    {
        setprompt(COMMAND);
    }
}


void MegaCmdExecuter::confirmDeleteAll()
{

    while (nodesToConfirmDelete.size())
    {
        MegaNode * nodeToConfirmDelete = nodesToConfirmDelete.front();
        nodesToConfirmDelete.erase(nodesToConfirmDelete.begin());
        doDeleteNode(nodeToConfirmDelete,api);
    }

    setprompt(COMMAND);
}

void MegaCmdExecuter::discardDeleteAll()
{
    while (nodesToConfirmDelete.size()){
        delete nodesToConfirmDelete.front();
        nodesToConfirmDelete.erase(nodesToConfirmDelete.begin());
    }
    setprompt(COMMAND);
}


void MegaCmdExecuter::doDeleteNode(MegaNode *nodeToDelete,MegaApi* api)
{
    char *nodePath = api->getNodePath(nodeToDelete);
    if (nodePath)
    {
        LOG_verbose << "Deleting: "<< nodePath;
    }
    else
    {
        LOG_warn << "Deleting node whose path could not be found " << nodeToDelete->getName();
    }
    MegaCmdListener *megaCmdListener = new MegaCmdListener(api, NULL);
    MegaNode *parent = api->getParentNode(nodeToDelete);
    if (parent && parent->getType() == MegaNode::TYPE_FILE)
    {
        api->removeVersion(nodeToDelete, megaCmdListener);
    }
    else
    {
        api->remove(nodeToDelete, megaCmdListener);
    }
    megaCmdListener->wait();
    string msj = "delete node ";
    if (nodePath)
    {
        msj += nodePath;
    }
    else
    {
        msj += nodeToDelete->getName();
    }
    checkNoErrors(megaCmdListener->getError(), msj);
    delete megaCmdListener;
    delete []nodePath;
    delete nodeToDelete;

}

int MegaCmdExecuter::deleteNodeVersions(MegaNode *nodeToDelete, MegaApi* api, int force)
{
    if (nodeToDelete->getType() == MegaNode::TYPE_FILE && api->getNumVersions(nodeToDelete) < 2)
    {
        if (!force)
        {
            LOG_err << "No versions found for " << nodeToDelete->getName();
        }
        return MCMDCONFIRM_YES; //nothing to do, no sense asking
    }

    int confirmationResponse;

    if (nodeToDelete->getType() != MegaNode::TYPE_FILE)
    {
        string confirmationQuery("Are you sure todelete the version histories of files within ");
        confirmationQuery += nodeToDelete->getName();
        confirmationQuery += "? (Yes/No): ";

        confirmationResponse = force?MCMDCONFIRM_ALL:askforConfirmation(confirmationQuery);

        if (confirmationResponse == MCMDCONFIRM_YES || confirmationResponse == MCMDCONFIRM_ALL)
        {
            MegaNodeList *children = api->getChildren(nodeToDelete);
            if (children)
            {
                for (int i = 0; i < children->size(); i++)
                {
                    MegaNode *child = children->get(i);
                    deleteNodeVersions(child, api, true);
                }
                delete children;
            }
        }
    }
    else
    {

        string confirmationQuery("Are you sure todelete the version histories of ");
        confirmationQuery += nodeToDelete->getName();
        confirmationQuery += "? (Yes/No): ";
        confirmationResponse = force?MCMDCONFIRM_ALL:askforConfirmation(confirmationQuery);

        if (confirmationResponse == MCMDCONFIRM_YES || confirmationResponse == MCMDCONFIRM_ALL)
        {

            MegaNodeList *versionsToDelete = api->getVersions(nodeToDelete);
            if (versionsToDelete)
            {
                for (int i = 0; i < versionsToDelete->size(); i++)
                {
                    MegaNode *versionNode = versionsToDelete->get(i);

                    if (versionNode->getHandle() != nodeToDelete->getHandle())
                    {
                        MegaCmdListener *megaCmdListener = new MegaCmdListener(NULL);
                        api->removeVersion(versionNode,megaCmdListener);
                        megaCmdListener->wait();
                        string fullname(versionNode->getName()?versionNode->getName():"NO_NAME");
                        fullname += "#";
                        fullname += SSTR(versionNode->getModificationTime());
                        if (checkNoErrors(megaCmdListener->getError(), "remove version: "+fullname))
                        {
                            LOG_verbose << " Removed " << fullname << " (" << getReadableTime(versionNode->getModificationTime()) << ")";
                        }
                        delete megaCmdListener;
                    }
                }
                delete versionsToDelete;
            }
        }
    }
    return confirmationResponse;
}

/**
 * @brief MegaCmdExecuter::deleteNode
 * @param nodeToDelete this function will delete this accordingly
 * @param api
 * @param recursive
 * @param force
 * @return confirmation code
 */
int MegaCmdExecuter::deleteNode(MegaNode *nodeToDelete, MegaApi* api, int recursive, int force)
{
    if (( nodeToDelete->getType() != MegaNode::TYPE_FILE ) && !recursive)
    {
        char *nodePath = api->getNodePath(nodeToDelete);
        setCurrentOutCode(MCMD_INVALIDTYPE);
        LOG_err << "Unable to delete folder: " << nodePath << ". Use -r to delete a folder recursively";
        delete nodeToDelete;
        delete []nodePath;
    }
    else
    {
        if (!getCurrentThreadIsCmdShell() && interactiveThread() && !force && nodeToDelete->getType() != MegaNode::TYPE_FILE)
        {
            bool alreadythere = false;
            for (std::vector< MegaNode * >::iterator it = nodesToConfirmDelete.begin(); it != nodesToConfirmDelete.end(); ++it)
            {
                if (((MegaNode*)*it)->getHandle() == nodeToDelete->getHandle())
                {
                    alreadythere= true;
                }
            }
            if (!alreadythere)
            {
                nodesToConfirmDelete.push_back(nodeToDelete);
                if (getprompt() != AREYOUSURETODELETE)
                {
                    string newprompt("Are you sure to delete ");
                    newprompt+=nodeToDelete->getName();
                    newprompt+=" ? (Yes/No/All/None): ";
                    setprompt(AREYOUSURETODELETE,newprompt);
                }
            }
            else
            {
                delete nodeToDelete;
            }

            return MCMDCONFIRM_NO; //default return
        }
        else if (!force && nodeToDelete->getType() != MegaNode::TYPE_FILE)
        {
            string confirmationQuery("Are you sure to delete ");
            confirmationQuery+=nodeToDelete->getName();
            confirmationQuery+=" ? (Yes/No/All/None): ";

            int confirmationResponse = askforConfirmation(confirmationQuery);

            if (confirmationResponse == MCMDCONFIRM_YES || confirmationResponse == MCMDCONFIRM_ALL)
            {
                LOG_debug << "confirmation received";
                doDeleteNode(nodeToDelete, api);
            }
            else
            {
                delete nodeToDelete;
                LOG_debug << "confirmation denied";
            }
            return confirmationResponse;
        }
        else //force
        {
            doDeleteNode(nodeToDelete, api);
            return MCMDCONFIRM_ALL;
        }
    }

    return MCMDCONFIRM_NO; //default return
}

void MegaCmdExecuter::downloadNode(string path, MegaApi* api, MegaNode *node, bool background, bool ignorequotawarn, int clientID, MegaCmdMultiTransferListener *multiTransferListener)
{
    if (sandboxCMD->isOverquota() && !ignorequotawarn)
    {
        time_t ts = time(NULL);
        // in order to speedup and not flood the server we only ask for the details every 1 minute or after account changes
        if (!sandboxCMD->temporalbandwidth || (ts - sandboxCMD->lastQuerytemporalBandwith ) > 60 )
        {
            LOG_verbose << " Updating temporal bandwith ";
            sandboxCMD->lastQuerytemporalBandwith = ts;

            MegaCmdListener *megaCmdListener = new MegaCmdListener(api, NULL);
            api->getExtendedAccountDetails(false, false, false, megaCmdListener);
            megaCmdListener->wait();

            if (checkNoErrors(megaCmdListener->getError(), "get account details"))
            {
                MegaAccountDetails *details = megaCmdListener->getRequest()->getMegaAccountDetails();
                sandboxCMD->istemporalbandwidthvalid = details->isTemporalBandwidthValid();
                if (details && details->isTemporalBandwidthValid())
                {
                    sandboxCMD->temporalbandwidth = details->getTemporalBandwidth();
                    sandboxCMD->temporalbandwithinterval = details->getTemporalBandwidthInterval();
                }
            }
            delete megaCmdListener;
        }

        OUTSTREAM << "Transfer not started. " << std::endl;
        if (sandboxCMD->istemporalbandwidthvalid)
        {
            OUTSTREAM << "You have utilized " << sizeToText(sandboxCMD->temporalbandwidth) << " of data transfer in the last "
                      << sandboxCMD->temporalbandwithinterval << " hours, "
                      "which took you over our current limit";
        }
        else
        {
            OUTSTREAM << "You have reached your bandwith quota";
        }
        OUTSTREAM << ". To circumvent this limit, "
                  "you can upgrade to Pro, which will give you your own bandwidth "
                  "package and also ample extra storage space. "
                     "Alternatively, you can try again in " << secondsToText(sandboxCMD->secondsOverQuota-(ts-sandboxCMD->timeOfOverquota)) <<
                     "." << std::endl << "See \"help --upgrade\" for further details" << std::endl;
        OUTSTREAM << "Use --ignore-quota-warn to initiate nevertheless" << std::endl;
        return;
    }

    if (!ignorequotawarn)
    {
        MegaCmdListener *megaCmdListener = new MegaCmdListener(api, NULL);
        api->queryTransferQuota(node->getSize(),megaCmdListener);
        megaCmdListener->wait();
        if (checkNoErrors(megaCmdListener->getError(), "query transfer quota"))
        {
            if (megaCmdListener->getRequest() && megaCmdListener->getRequest()->getFlag() )
            {
                OUTSTREAM << "Transfer not started: proceding will exceed transfer quota. "
                             "Use --ignore-quota-warn to initiate nevertheless" << std::endl;
                return;
            }
        }
        delete megaCmdListener;
    }

    MegaCmdTransferListener *megaCmdTransferListener = NULL;
    if (!background)
    {
        if (!multiTransferListener)
        {
            megaCmdTransferListener = new MegaCmdTransferListener(api, sandboxCMD, multiTransferListener, clientID);
        }
        multiTransferListener->onNewTransfer();
    }
#ifdef _WIN32
    replaceAll(path,"/","\\");
#endif
    LOG_debug << "Starting download: " << node->getName() << " to : " << path;

    if (multiTransferListener && !background)
    {
        api->startDownload(node, path.c_str(), multiTransferListener);
    }
    else
    {
        api->startDownload(node, path.c_str(), megaCmdTransferListener);
    }
    if (megaCmdTransferListener)
    {
        megaCmdTransferListener->wait();
#ifdef _WIN32
            Sleep(100); //give a while to print end of transfer
#endif
        if (checkNoErrors(megaCmdTransferListener->getError(), "download node"))
        {
            LOG_info << "Download complete: " << megaCmdTransferListener->getTransfer()->getPath();
        }
        delete megaCmdTransferListener;
    }
}

void MegaCmdExecuter::uploadNode(string path, MegaApi* api, MegaNode *node, string newname, bool background, bool ignorequotawarn, int clientID, MegaCmdMultiTransferListener *multiTransferListener)
{
    if (!ignorequotawarn)
    { //TODO: reenable this if ever queryBandwidthQuota applies to uploads as well
//        MegaCmdListener *megaCmdListener = new MegaCmdListener(api, NULL);
//        api->queryBandwidthQuota(node->getSize(),megaCmdListener);
//        megaCmdListener->wait();
//        if (checkNoErrors(megaCmdListener->getError(), "query bandwidth quota"))
//        {
//            if (megaCmdListener->getRequest() && megaCmdListener->getRequest()->getFlag() )
//            {
//                OUTSTREAM << "Transfer not started: proceding will exceed bandwith quota. "
//                             "Use --ignore-quota-warn to initiate nevertheless" << std::endl;
//                return;
//            }
//        }
    }
    MegaCmdTransferListener *megaCmdTransferListener = NULL;
    if (!background)
    {
        if (!multiTransferListener)
        {
            megaCmdTransferListener = new MegaCmdTransferListener(api, sandboxCMD, multiTransferListener, clientID);
        }
        multiTransferListener->onNewTransfer();
    }
    unescapeifRequired(path);

#ifdef _WIN32
    replaceAll(path,"/","\\");
#endif

    LOG_debug << "Starting upload: " << path << " to : " << node->getName() << (newname.size()?"/":"") << newname;


    MegaTransferListener *thelistener;
    if (multiTransferListener && !background)
    {
       thelistener = multiTransferListener;
    }
    else
    {
        thelistener = megaCmdTransferListener;
    }

    if (newname.size())
    {
        api->startUpload(path.c_str(), node, newname.c_str(), thelistener);
    }
    else
    {
        api->startUpload(path.c_str(), node, thelistener);
    }
    if (megaCmdTransferListener)
    {
        megaCmdTransferListener->wait();
#ifdef _WIN32
            Sleep(100); //give a while to print end of transfer
#endif
        if (megaCmdTransferListener->getError()->getErrorCode() == API_EREAD)
        {
            setCurrentOutCode(MCMD_NOTFOUND);
            LOG_err << "Could not find local path: " << path;
        }
        else if (checkNoErrors(megaCmdTransferListener->getError(), "Upload node"))
        {
            char * destinyPath = api->getNodePath(node);
            LOG_info << "Upload complete: " << path << " to " << destinyPath << newname;
            delete []destinyPath;
        }
        delete megaCmdTransferListener;
    }
}


void MegaCmdExecuter::exportNode(MegaNode *n, int64_t expireTime, bool force)
{
    bool copyrightAccepted = false;

    copyrightAccepted = ConfigurationManager::getConfigurationValue("copyrightAccepted", false) || force;
    if (!copyrightAccepted)
    {
        MegaNodeList * mnl = api->getPublicLinks();
        copyrightAccepted = mnl->size();
        delete mnl;
    }

    int confirmationResponse = copyrightAccepted?MCMDCONFIRM_YES:MCMDCONFIRM_NO;
    if (!copyrightAccepted)
    {
        string confirmationQuery("MEGA respects the copyrights of others and requires that users of the MEGA cloud service comply with the laws of copyright.\n"
                                 "You are strictly prohibited from using the MEGA cloud service to infringe copyrights.\n"
                                 "You may not upload, download, store, share, display, stream, distribute, email, link to, "
                                 "transmit or otherwise make available any files, data or content that infringes any copyright "
                                 "or other proprietary rights of any person or entity.");

        confirmationQuery+=" Do you accept this terms? (Yes/No): ";

        confirmationResponse = askforConfirmation(confirmationQuery);
    }

    if (confirmationResponse == MCMDCONFIRM_YES || confirmationResponse == MCMDCONFIRM_ALL)
    {
        ConfigurationManager::savePropertyValue("copyrightAccepted",true);
        MegaCmdListener *megaCmdListener = new MegaCmdListener(api, NULL);
        api->exportNode(n, expireTime, megaCmdListener);
        megaCmdListener->wait();
        if (checkNoErrors(megaCmdListener->getError(), "export node"))
        {
            MegaNode *nexported = api->getNodeByHandle(megaCmdListener->getRequest()->getNodeHandle());
            if (nexported)
            {
                char *nodepath = api->getNodePath(nexported);
                char *publiclink = nexported->getPublicLink();
                OUTSTREAM << "Exported " << nodepath << ": " << publiclink;
                if (nexported->getExpirationTime())
                {
                    OUTSTREAM << " expires at " << getReadableTime(nexported->getExpirationTime());
                }
                OUTSTREAM << std::endl;
                delete[] nodepath;
                delete[] publiclink;
                delete nexported;
            }
            else
            {
                setCurrentOutCode(MCMD_NOTFOUND);
                LOG_err << "Exported node not found!";
            }
        }
        delete megaCmdListener;
    }
}

void MegaCmdExecuter::disableExport(MegaNode *n)
{
    if (!n->isExported())
    {
        setCurrentOutCode(MCMD_INVALIDSTATE);
        LOG_err << "Could not disable export: node not exported.";
        return;
    }
    MegaCmdListener *megaCmdListener = new MegaCmdListener(api, NULL);

    api->disableExport(n, megaCmdListener);
    megaCmdListener->wait();
    if (checkNoErrors(megaCmdListener->getError(), "disable export"))
    {
        MegaNode *nexported = api->getNodeByHandle(megaCmdListener->getRequest()->getNodeHandle());
        if (nexported)
        {
            char *nodepath = api->getNodePath(nexported);
            OUTSTREAM << "Disabled export: " << nodepath << std::endl;
            delete[] nodepath;
            delete nexported;
        }
        else
        {
            setCurrentOutCode(MCMD_NOTFOUND);
            LOG_err << "Exported node not found!";
        }
    }

    delete megaCmdListener;
}

void MegaCmdExecuter::shareNode(MegaNode *n, string with, int level)
{
    MegaCmdListener *megaCmdListener = new MegaCmdListener(api, NULL);

    api->share(n, with.c_str(), level, megaCmdListener);
    megaCmdListener->wait();
    if (checkNoErrors(megaCmdListener->getError(), ( level != MegaShare::ACCESS_UNKNOWN ) ? "share node" : "disable share"))
    {
        MegaNode *nshared = api->getNodeByHandle(megaCmdListener->getRequest()->getNodeHandle());
        if (nshared)
        {
            char *nodepath = api->getNodePath(nshared);
            if (megaCmdListener->getRequest()->getAccess() == MegaShare::ACCESS_UNKNOWN)
            {
                OUTSTREAM << "Stopped sharing " << nodepath << " with " << megaCmdListener->getRequest()->getEmail() << std::endl;
            }
            else
            {
                OUTSTREAM << "Shared " << nodepath << " : " << megaCmdListener->getRequest()->getEmail()
                          << " accessLevel=" << megaCmdListener->getRequest()->getAccess() << std::endl;
            }
            delete[] nodepath;
            delete nshared;
        }
        else
        {
            setCurrentOutCode(MCMD_NOTFOUND);
            LOG_err << "Shared node not found!";
        }
    }

    delete megaCmdListener;
}

void MegaCmdExecuter::disableShare(MegaNode *n, string with)
{
    shareNode(n, with, MegaShare::ACCESS_UNKNOWN);
}

int MegaCmdExecuter::makedir(string remotepath, bool recursive, MegaNode *parentnode)
{
    MegaNode *currentnode;
    if (parentnode)
    {
        currentnode = parentnode;
    }
    else
    {
        currentnode = api->getNodeByHandle(cwd);
    }
    if (currentnode)
    {
        string rest = remotepath;
        while (rest.length())
        {
            bool lastleave = false;
            size_t possep = rest.find_first_of("/");
            if (possep == string::npos)
            {
                possep = rest.length();
                lastleave = true;
            }

            string newfoldername = rest.substr(0, possep);
            if (!rest.length())
            {
                break;
            }
            if (newfoldername.length())
            {
                MegaNode *existing_node = api->getChildNode(currentnode, newfoldername.c_str());
                if (!existing_node)
                {
                    if (!recursive && !lastleave)
                    {
                        LOG_err << "Use -p to create folders recursively";
                        if (currentnode != parentnode)
                            delete currentnode;
                        return MCMD_EARGS;
                    }
                    LOG_verbose << "Creating (sub)folder: " << newfoldername;
                    MegaCmdListener *megaCmdListener = new MegaCmdListener(NULL);
                    api->createFolder(newfoldername.c_str(), currentnode, megaCmdListener);
                    actUponCreateFolder(megaCmdListener);
                    delete megaCmdListener;
                    MegaNode *prevcurrentNode = currentnode;
                    currentnode = api->getChildNode(currentnode, newfoldername.c_str());
                    if (prevcurrentNode != parentnode)
                        delete prevcurrentNode;
                    if (!currentnode)
                    {
                        LOG_err << "Couldn't get node for created subfolder: " << newfoldername;
                        if (currentnode != parentnode)
                            delete currentnode;
                        return MCMD_INVALIDSTATE;
                    }
                }
                else
                {
                    if (currentnode != parentnode)
                        delete currentnode;
                    currentnode = existing_node;
                }

                if (lastleave && existing_node)
                {
                    LOG_err << ((existing_node->getType() == MegaNode::TYPE_FILE)?"File":"Folder") << " already exists: " << remotepath;
                    if (currentnode != parentnode)
                        delete currentnode;
                    return MCMD_INVALIDSTATE;
                }
            }

            //string rest = rest.substr(possep+1,rest.length()-possep-1);
            if (!lastleave)
            {
                rest = rest.substr(possep + 1, rest.length());
            }
            else
            {
                break;
            }
        }
        if (currentnode != parentnode)
            delete currentnode;
    }
    else
    {
        return MCMD_EARGS;
    }
    return MCMD_OK;

}


string MegaCmdExecuter::getCurrentPath()
{
    string toret;
    MegaNode *ncwd = api->getNodeByHandle(cwd);
    if (ncwd)
    {
        char *currentPath = api->getNodePath(ncwd);
        toret = string(currentPath);
        delete []currentPath;
        delete ncwd;
    }
    return toret;
}

long long MegaCmdExecuter::getVersionsSize(MegaNode *n)
{
    long long toret = 0;

    MegaNodeList *versionNodes = api->getVersions(n);
    if (versionNodes)
    {
        for (int i = 0; i < versionNodes->size(); i++)
        {
            MegaNode *versionNode = versionNodes->get(i);
            toret += api->getSize(versionNode);
        }
    }

    MegaNodeList *children = api->getChildren(n);
    if (children)
    {
        for (int i = 0; i < children->size(); i++)
        {
            MegaNode *child = children->get(i);
            toret += getVersionsSize(child);
        }
        delete children;
    }
    return toret;
}

vector<string> MegaCmdExecuter::listpaths(bool usepcre, string askedPath, bool discardFiles)
{
    vector<string> paths;
    if ((int)askedPath.size())
    {
        vector<string> *pathsToList = nodesPathsbypath(askedPath.c_str(), usepcre);
        if (pathsToList)
        {
            for (std::vector< string >::iterator it = pathsToList->begin(); it != pathsToList->end(); ++it)
            {
                string nodepath= *it;
                MegaNode *ncwd = api->getNodeByHandle(cwd);
                if (ncwd)
                {
                    MegaNode * n = nodebypath(nodepath.c_str());
                    if (n)
                    {
                        if (n->getType() != MegaNode::TYPE_FILE)
                        {
                            nodepath += "/";
                        }
                        if (!( discardFiles && ( n->getType() == MegaNode::TYPE_FILE )))
                        {
                            paths.push_back(nodepath);
                        }

                        delete n;
                    }
                    else
                    {
                        LOG_debug << "Unexpected: matching path has no associated node: " << nodepath << ". Could have been deleted in the process";
                    }
                    delete ncwd;
                }
                else
                {
                    setCurrentOutCode(MCMD_INVALIDSTATE);
                    LOG_err << "Couldn't find woking folder (it might been deleted)";
                }
            }
            pathsToList->clear();
            delete pathsToList;
        }
    }

    return paths;
}

vector<string> MegaCmdExecuter::getlistusers()
{
    vector<string> users;

    MegaUserList* usersList = api->getContacts();
    if (usersList)
    {
        for (int i = 0; i < usersList->size(); i++)
        {
            users.push_back(usersList->get(i)->getEmail());
        }

        delete usersList;
    }
    return users;
}

vector<string> MegaCmdExecuter::getNodeAttrs(string nodePath)
{
    vector<string> attrs;

    MegaNode *n = nodebypath(nodePath.c_str());
    if (n)
    {
        //List node custom attributes
        MegaStringList *attrlist = n->getCustomAttrNames();
        if (attrlist)
        {
            for (int a = 0; a < attrlist->size(); a++)
            {
                attrs.push_back(attrlist->get(a));
            }

            delete attrlist;
        }
        delete n;
    }
    return attrs;
}

vector<string> MegaCmdExecuter::getUserAttrs()
{
    vector<string> attrs;
    for (int i=0;i < 10; i++)
    {
        attrs.push_back(getAttrStr(i) );
    }
    return attrs;
}

vector<string> MegaCmdExecuter::getsessions()
{
    vector<string> sessions;
    MegaCmdListener *megaCmdListener = new MegaCmdListener(NULL);
    api->getExtendedAccountDetails(true, true, true, megaCmdListener);
    int trywaitout = megaCmdListener->trywait(3000);
    if (trywaitout)
    {
        return sessions;
    }

    if (checkNoErrors(megaCmdListener->getError(), "get sessions"))
    {
        MegaAccountDetails *details = megaCmdListener->getRequest()->getMegaAccountDetails();
        if (details)
        {
            int numSessions = details->getNumSessions();
            for (int i = 0; i < numSessions; i++)
            {
                MegaAccountSession * session = details->getSession(i);
                if (session)
                {
                    if (session->isAlive())
                    {
                        sessions.push_back(api->userHandleToBase64(session->getHandle()));
                    }
                    delete session;
                }
            }

            delete details;
        }
    }
    delete megaCmdListener;
    return sessions;
}

void MegaCmdExecuter::signup(string name, string passwd, string email)
{
    MegaCmdListener *megaCmdListener = new MegaCmdListener(NULL);
    api->createAccount(email.c_str(), passwd.c_str(), name.c_str(), megaCmdListener);
    megaCmdListener->wait();
    if (checkNoErrors(megaCmdListener->getError(), "create account <" + email + ">"))
    {
        OUTSTREAM << "Account <" << email << "> created succesfully. You will receive a confirmation link. Use \"confirm\" with the provided link to confirm that account" << std::endl;
    }

    delete megaCmdListener;

    MegaCmdListener *megaCmdListener2 = new MegaCmdListener(NULL);
    api->localLogout(megaCmdListener2);
    megaCmdListener2->wait();
    checkNoErrors(megaCmdListener2->getError(), "logging out from ephemeral account");
    delete megaCmdListener2;
}

void MegaCmdExecuter::signupWithPassword(string passwd)
{
    return signup(name, passwd, login);
}

void MegaCmdExecuter::confirm(string passwd, string email, string link)
{
    MegaCmdListener *megaCmdListener2 = new MegaCmdListener(NULL);
    api->confirmAccount(link.c_str(), passwd.c_str(), megaCmdListener2);
    megaCmdListener2->wait();
    if (megaCmdListener2->getError()->getErrorCode() == MegaError::API_ENOENT)
    {
        LOG_err << "Invalid password";
    }
    else if (checkNoErrors(megaCmdListener2->getError(), "confirm account"))
    {
        OUTSTREAM << "Account " << email << " confirmed succesfully. You can login with it now" << std::endl;
    }

    delete megaCmdListener2;
}

void MegaCmdExecuter::confirmWithPassword(string passwd)
{
    return confirm(passwd, login, link);
}


bool MegaCmdExecuter::IsFolder(string path)
{
#ifdef _WIN32
    replaceAll(path,"/","\\");
#endif
    string localpath;
    fsAccessCMD->path2local(&path, &localpath);
    FileAccess *fa = fsAccessCMD->newfileaccess();
    bool destinyIsFolder = fa->isfolder(&localpath);
    delete fa;
    return destinyIsFolder;
}

void MegaCmdExecuter::printTransfersHeader(const unsigned int PATHSIZE, bool printstate)
{
    OUTSTREAM << "DIR/SYNC TAG  " << getFixLengthString("SOURCEPATH ",PATHSIZE) << getFixLengthString("DESTINYPATH ",PATHSIZE)
              << "  " << getFixLengthString("    PROGRESS",21);
    if (printstate)
    {
        OUTSTREAM  << "  " << "STATE";
    }
    OUTSTREAM << std::endl;
}

void MegaCmdExecuter::printTransfer(MegaTransfer *transfer, const unsigned int PATHSIZE, bool printstate)
{
    //Direction
#ifdef _WIN32
    OUTSTREAM << " " << ((transfer->getType() == MegaTransfer::TYPE_DOWNLOAD)?"D":"U") << " ";
#else
    OUTSTREAM << " " << ((transfer->getType() == MegaTransfer::TYPE_DOWNLOAD)?"\u21d3":"\u21d1") << " ";
#endif
    //TODO: handle TYPE_LOCAL_HTTP_DOWNLOAD

    //type (transfer/normal)
    if (transfer->isSyncTransfer())
    {
#ifdef _WIN32
        OUTSTREAM << "S";
#else
        OUTSTREAM << "\u21f5";
#endif
    }
    else
    {
        OUTSTREAM << " " ;
    }

    OUTSTREAM << " " ;

    //tag
    OUTSTREAM << getRightAlignedString(SSTR(transfer->getTag()),7) << " ";

    if (transfer->getType() == MegaTransfer::TYPE_DOWNLOAD)
    {
        // source
        MegaNode * node = api->getNodeByHandle(transfer->getNodeHandle());
        if (node)
        {
            char * nodepath = api->getNodePath(node);
            OUTSTREAM << getFixLengthString(nodepath,PATHSIZE);
            delete []nodepath;

            delete node;
        }
        else
        {
            globalTransferListener->completedTransfersMutex.lock();
            OUTSTREAM << getFixLengthString(globalTransferListener->completedPathsByHandle[transfer->getNodeHandle()],PATHSIZE);
            globalTransferListener->completedTransfersMutex.unlock();
        }

        OUTSTREAM << " ";

        //destination
        string dest = transfer->getParentPath();
        dest.append(transfer->getFileName());
        OUTSTREAM << getFixLengthString(dest,PATHSIZE);
    }
    else
    {

        //source
        string source(transfer->getParentPath()?transfer->getParentPath():"");
        source.append(transfer->getFileName());

        OUTSTREAM << getFixLengthString(source, PATHSIZE);
        OUTSTREAM << " ";

        //destination
        MegaNode * parentNode = api->getNodeByHandle(transfer->getParentHandle());
        if (parentNode)
        {
            char * parentnodepath = api->getNodePath(parentNode);
            OUTSTREAM << getFixLengthString(parentnodepath ,PATHSIZE);
            delete []parentnodepath;

            delete parentNode;
        }
        else
        {
            OUTSTREAM << getFixLengthString("",PATHSIZE,'-');
            LOG_warn << "Could not find destination (parent handle "<< ((transfer->getParentHandle()==INVALID_HANDLE)?" invalid":" valid")
                     <<" ) for upload transfer. Source=" << transfer->getParentPath() << transfer->getFileName();
        }
    }

    //progress
    float percent;
    if (transfer->getTotalBytes() == 0)
    {
        percent = 0;
    }
    else
    {
        percent = float(transfer->getTransferredBytes()*1.0/transfer->getTotalBytes());
    }
    OUTSTREAM << "  " << getFixLengthString(percentageToText(percent),7,' ',true)
              << " of " << getFixLengthString(sizeToText(transfer->getTotalBytes()),10,' ',true);

    //state
    if (printstate)
    {
        OUTSTREAM << "  " << getTransferStateStr(transfer->getState());
    }

    OUTSTREAM << std::endl;
}

void MegaCmdExecuter::printSyncHeader(const unsigned int PATHSIZE)
{
    OUTSTREAM << "ID ";
    OUTSTREAM << getFixLengthString("LOCALPATH ", PATHSIZE) << " ";
    OUTSTREAM << getFixLengthString("REMOTEPATH ", PATHSIZE) << " ";

    OUTSTREAM << getFixLengthString("ActState", 10) << " ";
    OUTSTREAM << getFixLengthString("SyncState", 9) << " ";
    OUTSTREAM << getRightAlignedString("SIZE", 8) << " ";
    OUTSTREAM << getRightAlignedString("FILES", 6) << " ";
    OUTSTREAM << getRightAlignedString("DIRS", 6);
    OUTSTREAM << std::endl;

}

#ifdef ENABLE_BACKUPS

void MegaCmdExecuter::printBackupHeader(const unsigned int PATHSIZE)
{
    OUTSTREAM << "TAG  " << " ";
    OUTSTREAM << getFixLengthString("LOCALPATH ", PATHSIZE) << " ";
    OUTSTREAM << getFixLengthString("REMOTEPARENTPATH ", PATHSIZE) << " ";
    OUTSTREAM << getRightAlignedString("STATUS", 14);
    OUTSTREAM << std::endl;
}


void MegaCmdExecuter::printBackupSummary(int tag, const char * localfolder, const char *remoteparentfolder, string status, const unsigned int PATHSIZE)
{
    OUTSTREAM << getFixLengthString(SSTR(tag),5) << " "
              << getFixLengthString(localfolder, PATHSIZE) << " "
              << getFixLengthString((remoteparentfolder?remoteparentfolder:"INVALIDPATH"), PATHSIZE) << " "
              << getRightAlignedString(status, 14)
              << std::endl;
}

void MegaCmdExecuter::printBackupDetails(MegaBackup *backup)
{
    if (backup)
    {
        string speriod = (backup->getPeriod() == -1)?backup->getPeriodString():getReadablePeriod(backup->getPeriod()/10);
        OUTSTREAM << "  Max Backups:   " << backup->getMaxBackups() << std::endl;
        OUTSTREAM << "  Period:         " << "\"" << speriod << "\"" << std::endl;
        OUTSTREAM << "  Next backup scheduled for: " << getReadableTime(backup->getNextStartTime());

        OUTSTREAM << std::endl;
        OUTSTREAM << "  " << " -- CURRENT/LAST BACKUP --" << std::endl;
        OUTSTREAM << "  " << getFixLengthString("FILES UP/TOT", 15);
        OUTSTREAM << "  " << getFixLengthString("FOLDERS CREATED", 15);
        OUTSTREAM << "  " << getRightAlignedString("PROGRESS", 10);
        OUTSTREAM << std::endl;

        string sfiles = SSTR(backup->getNumberFiles()) + "/" + SSTR(backup->getTotalFiles());
        OUTSTREAM << "  " << getRightAlignedString(sfiles, 8) << "       ";
        OUTSTREAM << "  " << getRightAlignedString(SSTR(backup->getNumberFolders()), 8) << "       ";
        long long trabytes = backup->getTransferredBytes();
        long long totbytes = backup->getTotalBytes();
        double percent = totbytes?double(trabytes)/double(totbytes):0;

        string sprogress = sizeProgressToText(trabytes, totbytes) + "  " + percentageToText(percent);
        OUTSTREAM << "  " << getRightAlignedString(sprogress,10);
        OUTSTREAM << std::endl;
    }
}

void MegaCmdExecuter::printBackupHistory(MegaBackup *backup, MegaNode *parentnode, const unsigned int PATHSIZE)
{
    bool firstinhistory = true;
    MegaStringList *msl = api->getBackupFolders(backup->getTag());
    if (msl)
    {
        for (int i = 0; i < msl->size(); i++)
        {
            if (firstinhistory)
            {
                OUTSTREAM << "  " << " -- SAVED BACKUPS --" << std::endl;

                // print header
                OUTSTREAM << "  " << getFixLengthString("NAME", PATHSIZE) << " ";
                OUTSTREAM << getFixLengthString("DATE", 18) << " ";
                OUTSTREAM << getRightAlignedString("STATUS", 11)<< " ";
                OUTSTREAM << getRightAlignedString("FILES", 6)<< " ";
                OUTSTREAM << getRightAlignedString("FOLDERS", 7);
                OUTSTREAM << std::endl;

                firstinhistory = false;
            }

            string bpath = msl->get(i);
            size_t pos = bpath.find("_bk_");
            string btime = "";
            if (pos != string::npos)
            {
                btime = bpath.substr(pos+4);
            }

            pos = bpath.find_last_of("/\\");
            string backupInstanceName = bpath;
            if (pos != string::npos)
            {
                backupInstanceName = bpath.substr(pos+1);
            }

            string printableDate = "UNKNOWN";
            if (btime.size())
            {
                struct tm dt;
                fillStructWithSYYmdHMS(btime,dt);
                printableDate = getReadableShortTime(mktime(&dt));
            }

            string backupInstanceStatus="NOT_FOUND";
            long long nfiles = 0;
            long long nfolders = 0;
            if (parentnode)
            {
                MegaNode *backupInstanceNode = nodebypath(msl->get(i));
                if (backupInstanceNode)
                {
                    backupInstanceStatus = backupInstanceNode->getCustomAttr("BACKST");

                    getNumFolderFiles(backupInstanceNode, api, &nfiles, &nfolders);

                }

                delete backupInstanceNode;
            }

            OUTSTREAM << "  " << getFixLengthString(backupInstanceName, PATHSIZE) << " ";
            OUTSTREAM << getFixLengthString(printableDate, 18) << " ";
            OUTSTREAM << getRightAlignedString(backupInstanceStatus, 11) << " ";
            OUTSTREAM << getRightAlignedString(SSTR(nfiles), 6)<< " ";
            OUTSTREAM << getRightAlignedString(SSTR(nfolders), 7);
            //OUTSTREAM << getRightAlignedString("PROGRESS", 10);// some info regarding progress or the like in case of failure could be interesting. Although we don't know total files/folders/bytes
            OUTSTREAM << std::endl;

        }
        delete msl;
    }
}

void MegaCmdExecuter::printBackup(int tag, MegaBackup *backup, const unsigned int PATHSIZE, bool extendedinfo, bool showhistory, MegaNode *parentnode)
{
    if (backup)
    {
        const char *nodepath = NULL;
        bool deleteparentnode = false;

        if (!parentnode)
        {
            parentnode = api->getNodeByHandle(backup->getMegaHandle());
            if (parentnode)
            {
                nodepath = api->getNodePath(parentnode);
                deleteparentnode = true;
            }
        }
        else
        {
            nodepath = api->getNodePath(parentnode);
        }

        printBackupSummary(tag, backup->getLocalFolder(),nodepath,backupSatetStr(backup->getState()), PATHSIZE);
        if (extendedinfo)
        {
            printBackupDetails(backup);
        }
        delete []nodepath;

        if (showhistory && parentnode)
        {
            printBackupHistory(backup, parentnode, PATHSIZE);
        }

        if (deleteparentnode)
        {
            delete parentnode;
        }
    }
    else
    {
        OUTSTREAM << "BACKUP not found " << std::endl;
    }
}

void MegaCmdExecuter::printBackup(backup_struct *backupstruct, const unsigned int PATHSIZE, bool extendedinfo, bool showhistory)
{
    if (backupstruct->tag >= 0)
    {
        MegaBackup *backup = api->getBackupByTag(backupstruct->tag);
        if (backup)
        {
            printBackup(backupstruct->tag, backup, PATHSIZE, extendedinfo, showhistory);
            delete backup;
        }
        else
        {
            OUTSTREAM << "BACKUP not found: " << backupstruct->tag << std::endl;
        }
    }
    else
    { //merely print configuration
        printBackupSummary(backupstruct->tag, backupstruct->localpath.c_str(),"UNKOWN"," FAILED", PATHSIZE);
        if (extendedinfo)
        {
            string speriod = (backupstruct->period == -1)?backupstruct->speriod:getReadablePeriod(backupstruct->period/10);
            OUTSTREAM << "         Period: " << "\"" << speriod << "\"" << std::endl;
            OUTSTREAM << "   Max. Backups: " << backupstruct->numBackups << std::endl;
        }
    }
}
#endif

void MegaCmdExecuter::printSync(int i, string key, const char *nodepath, sync_struct * thesync, MegaNode *n, long long nfiles, long long nfolders, const unsigned int PATHSIZE)
{
    //tag
    OUTSTREAM << getRightAlignedString(SSTR(i),2) << " ";

    OUTSTREAM << getFixLengthString(key,PATHSIZE) << " ";

    OUTSTREAM << getFixLengthString(nodepath,PATHSIZE) << " ";

    string sstate(key);
    sstate = rtrim(sstate, '/');
#ifdef _WIN32
    sstate = rtrim(sstate, '\\');
#endif
    string psstate;
    fsAccessCMD->path2local(&sstate,&psstate);
    int statepath = api->syncPathState(&psstate);

    MegaSync *msync = api->getSyncByNode(n);
    string syncstate = "REMOVED";
    if (msync)
    {
        syncstate = getSyncStateStr(msync->getState());
    }

    string statetoprint;
    if (thesync->active)
    {
        statetoprint = syncstate;
    }
    else
    {
        if (msync)
        {
            statetoprint = "Disabling:";
            statetoprint+=syncstate;
        }
        else
        {
            statetoprint = "Disabled";
        }
    }
    delete msync;

    OUTSTREAM << getFixLengthString(statetoprint,10) << " ";
    OUTSTREAM << getFixLengthString(getSyncPathStateStr(statepath),9) << " ";

    OUTSTREAM << getRightAlignedString(sizeToText(api->getSize(n), false),8) << " ";

    OUTSTREAM << getRightAlignedString(SSTR(nfiles),6) << " ";
    OUTSTREAM << getRightAlignedString(SSTR(nfolders),6) << " ";

    OUTSTREAM << std::endl;

}

void MegaCmdExecuter::doFind(MegaNode* nodeBase, string word, int printfileinfo, string pattern, bool usepcre, time_t minTime, time_t maxTime, int64_t minSize, int64_t maxSize)
{
    struct criteriaNodeVector pnv;
    pnv.pattern = pattern;

    vector<MegaNode *> listOfMatches;
    pnv.nodesMatching = &listOfMatches;
    pnv.usepcre = usepcre;

    pnv.minTime = minTime;
    pnv.maxTime = maxTime;
    pnv.minSize = minSize;
    pnv.maxSize = maxSize;


    processTree(nodeBase, includeIfMatchesCriteria, (void*)&pnv);


    for (std::vector< MegaNode * >::iterator it = listOfMatches.begin(); it != listOfMatches.end(); ++it)
    {
        MegaNode * n = *it;
        if (n)
        {
            string pathToShow;

            if ( word.size() > 0 && ( (word.find("/") == 0) || (word.find("..") != string::npos)) )
            {
                char * nodepath = api->getNodePath(n);
                pathToShow = string(nodepath);
                delete [] nodepath;
            }
            else
            {
                pathToShow = getDisplayPath("", n);
            }
            if (printfileinfo)
            {
                dumpNode(n, 3, false, 1, pathToShow.c_str());
            }
            else
            {
                OUTSTREAM << pathToShow << std::endl;
            }
            //notice: some nodes may be dumped twice

            delete n;
        }
    }

    listOfMatches.clear();
}

string MegaCmdExecuter::getLPWD()
{
    string relativePath = ".";
    string absolutePath = "Unknown";
    string localRelativePath;
    fsAccessCMD->path2local(&relativePath, &localRelativePath);
    string localAbsolutePath;
    if (fsAccessCMD->expanselocalpath(&localRelativePath, &localAbsolutePath))
    {
        fsAccessCMD->local2path(&localAbsolutePath, &absolutePath);
    }

    return absolutePath;
}


void MegaCmdExecuter::move(MegaNode * n, string destiny)
{
    MegaNode* tn; //target node
    string newname;

    // source node must exist
    if (!n)
    {
        return;
    }


    char * nodepath = api->getNodePath(n);
    LOG_debug << "Moving : " << nodepath << " to " << destiny;
    delete []nodepath;

    // we have four situations:
    // 1. target path does not exist - fail
    // 2. target node exists and is folder - move
    // 3. target node exists and is file - delete and rename (unless same)
    // 4. target path exists, but filename does not - rename
    if (( tn = nodebypath(destiny.c_str(), NULL, &newname)))
    {
        if (tn->getHandle() == n->getHandle())
        {
            LOG_err << "Source and destiny are the same";
        }
        else
        {
            if (newname.size()) //target not found, but tn has what was before the last "/" in the path.
            {
                if (tn->getType() == MegaNode::TYPE_FILE)
                {
                    setCurrentOutCode(MCMD_INVALIDTYPE);
                    LOG_err << destiny << ": Not a directory";
                    delete tn;
                    delete n;
                    return;
                }
                else //move and rename!
                {
                    MegaCmdListener *megaCmdListener = new MegaCmdListener(NULL);
                    api->moveNode(n, tn, megaCmdListener);
                    megaCmdListener->wait();
                    if (checkNoErrors(megaCmdListener->getError(), "move"))
                    {
                        MegaCmdListener *megaCmdListener = new MegaCmdListener(NULL);
                        api->renameNode(n, newname.c_str(), megaCmdListener);
                        megaCmdListener->wait();
                        checkNoErrors(megaCmdListener->getError(), "rename");
                        delete megaCmdListener;
                    }
                    else
                    {
                        LOG_debug << "Won't rename, since move failed " << n->getName() << " to " << tn->getName() << " : " << megaCmdListener->getError()->getErrorCode();
                    }
                    delete megaCmdListener;
                }
            }
            else //target found
            {
                if (tn->getType() == MegaNode::TYPE_FILE) //move & remove old & rename new
                {
                    // (there should never be any orphaned filenodes)
                    MegaNode *tnParentNode = api->getNodeByHandle(tn->getParentHandle());
                    if (tnParentNode)
                    {
                        delete tnParentNode;

                        //move into the parent of target node
                        MegaCmdListener *megaCmdListener = new MegaCmdListener(NULL);
                        api->moveNode(n, api->getNodeByHandle(tn->getParentHandle()), megaCmdListener);
                        megaCmdListener->wait();
                        if (checkNoErrors(megaCmdListener->getError(), "move node"))
                        {
                            const char* name_to_replace = tn->getName();

                            //remove (replaced) target node
                            if (n != tn) //just in case moving to same location
                            {
                                MegaCmdListener *megaCmdListener = new MegaCmdListener(NULL);
                                api->remove(tn, megaCmdListener); //remove target node
                                megaCmdListener->wait();
                                if (!checkNoErrors(megaCmdListener->getError(), "remove target node"))
                                {
                                    LOG_err << "Couldnt move " << n->getName() << " to " << tn->getName() << " : " << megaCmdListener->getError()->getErrorCode();
                                }
                                delete megaCmdListener;
                            }

                            // rename moved node with the new name
                            if (!strcmp(name_to_replace, n->getName()))
                            {
                                MegaCmdListener *megaCmdListener = new MegaCmdListener(NULL);
                                api->renameNode(n, name_to_replace, megaCmdListener);
                                megaCmdListener->wait();
                                if (!checkNoErrors(megaCmdListener->getError(), "rename moved node"))
                                {
                                    LOG_err << "Failed to rename moved node: " << megaCmdListener->getError()->getErrorString();
                                }
                                delete megaCmdListener;
                            }
                        }
                        delete megaCmdListener;
                    }
                    else
                    {
                        setCurrentOutCode(MCMD_INVALIDSTATE);
                        LOG_fatal << "Destiny node is orphan!!!";
                    }
                }
                else // target is a folder
                {
                    MegaCmdListener *megaCmdListener = new MegaCmdListener(NULL);
                    api->moveNode(n, tn, megaCmdListener);
                    megaCmdListener->wait();
                    checkNoErrors(megaCmdListener->getError(), "move node");
                    delete megaCmdListener;
                }
            }
        }
        delete tn;
    }
    else //target not found (not even its folder), cant move
    {
        setCurrentOutCode(MCMD_NOTFOUND);
        LOG_err << destiny << ": No such directory";
    }
}


bool MegaCmdExecuter::isValidFolder(string destiny)
{
    bool isdestinyavalidfolder = true;
    MegaNode *ndestiny = nodebypath(destiny.c_str());;
    if (ndestiny)
    {
        if (ndestiny->getType() == MegaNode::TYPE_FILE)
        {
            isdestinyavalidfolder = false;
        }
        delete ndestiny;
    }
    else
    {
        isdestinyavalidfolder = false;
    }
    return isdestinyavalidfolder;
}

void MegaCmdExecuter::restartsyncs()
{
    map<string, sync_struct *>::iterator itr;
    for (itr = ConfigurationManager::configuredSyncs.begin(); itr != ConfigurationManager::configuredSyncs.end(); ++itr)
    {
        string key = ( *itr ).first;
        sync_struct *thesync = ((sync_struct*)( *itr ).second );
        if (thesync->active)
        {
            MegaNode * n = api->getNodeByHandle(thesync->handle);
            if (n)
            {
                char * nodepath = api->getNodePath(n);
                LOG_info << "Restarting sync "<< key << ": " << nodepath;
                delete []nodepath;
                MegaCmdListener *megaCmdListener = new MegaCmdListener(NULL);
                api->disableSync(n, megaCmdListener);
                megaCmdListener->wait();

                if (checkNoErrors(megaCmdListener->getError(), "stop sync"))
                {
                    thesync->active = false;

                    MegaSync *msync = api->getSyncByNode(n);
                    if (!msync)
                    {
                        MegaCmdListener *megaCmdListener2 = new MegaCmdListener(NULL);
                        api->syncFolder(thesync->localpath.c_str(), n, megaCmdListener2);
                        megaCmdListener2->wait();

                        if (checkNoErrors(megaCmdListener2->getError(), "resume sync"))
                        {
                            thesync->active = true;
                            thesync->loadedok = true;

                            if (megaCmdListener2->getRequest()->getNumber())
                            {
                                thesync->fingerprint = megaCmdListener2->getRequest()->getNumber();
                            }
                        }
                        else
                        {
                            thesync->active = false;
                            thesync->loadedok = false;
                        }
                        delete megaCmdListener2;
                        delete msync;
                    }
                    else
                    {
                        setCurrentOutCode(MCMD_INVALIDSTATE);
                        LOG_err << "Failed to restart sync: " << key << ". You will need to manually reenable or restart MEGAcmd";
                    }
                }
                delete megaCmdListener;
                delete n;
            }
        }
    }
}

#ifdef ENABLE_BACKUPS
bool MegaCmdExecuter::establishBackup(string pathToBackup, MegaNode *n, int64_t period, string speriod,  int numBackups)
{
    bool attendpastbackups = true; //TODO: receive as parameter
    static int backupcounter = 0;
    string path;
    string localrelativepath;
    string localabsolutepath;
    fsAccessCMD->path2local(&pathToBackup, &localrelativepath);
    fsAccessCMD->expanselocalpath(&localrelativepath, &localabsolutepath);
    fsAccessCMD->local2path(&localabsolutepath, &path);

    MegaCmdListener *megaCmdListener = new MegaCmdListener(api, NULL);
    api->setBackup(path.c_str(), n, attendpastbackups, period, speriod.c_str(), numBackups, megaCmdListener);
    megaCmdListener->wait();
    if (checkNoErrors(megaCmdListener->getError(), "establish backup"))
    {
        mtxBackupsMap.lock();

        backup_struct *thebackup = NULL;
        if (ConfigurationManager::configuredBackups.find(megaCmdListener->getRequest()->getFile()) != ConfigurationManager::configuredBackups.end())
        {
            thebackup = ConfigurationManager::configuredBackups[megaCmdListener->getRequest()->getFile()];
            if (thebackup->id == -1)
            {
                thebackup->id = backupcounter++;
            }
        }
        else
        {
            thebackup = new backup_struct;
            thebackup->id = backupcounter++;
            ConfigurationManager::configuredBackups[megaCmdListener->getRequest()->getFile()] = thebackup;
        }
        thebackup->active = true;
        thebackup->handle = megaCmdListener->getRequest()->getNodeHandle();
        thebackup->localpath = string(megaCmdListener->getRequest()->getFile());
        thebackup->numBackups = numBackups;
        thebackup->period = period;
        thebackup->speriod = speriod;
        thebackup->failed = false;
        thebackup->tag = megaCmdListener->getRequest()->getTransferTag();

        char * nodepath = api->getNodePath(n);
        LOG_info << "Added backup: " << megaCmdListener->getRequest()->getFile() << " to " << nodepath;
        mtxBackupsMap.unlock();
        delete []nodepath;
        delete megaCmdListener;

        return true;
    }
    else
    {
        bool foundbytag = false;
        // find by tag in configured (modification failed)
        for (std::map<std::string, backup_struct *>::iterator itr = ConfigurationManager::configuredBackups.begin();
             itr != ConfigurationManager::configuredBackups.end(); itr++)
        {
            if (itr->second->tag == megaCmdListener->getRequest()->getTransferTag())
            {
                backup_struct *thebackup = itr->second;

                foundbytag = true;
                thebackup->handle = megaCmdListener->getRequest()->getNodeHandle();
                thebackup->localpath = string(megaCmdListener->getRequest()->getFile());
                thebackup->numBackups = megaCmdListener->getRequest()->getNumRetry();
                thebackup->period = megaCmdListener->getRequest()->getNumber();
                thebackup->speriod = string(megaCmdListener->getRequest()->getText());;
                thebackup->failed = true;
            }
        }


        if (!foundbytag)
        {
            std::map<std::string, backup_struct *>::iterator itr = ConfigurationManager::configuredBackups.find(megaCmdListener->getRequest()->getFile());
            if ( itr != ConfigurationManager::configuredBackups.end())
            {
                if (megaCmdListener->getError()->getErrorCode() != MegaError::API_EEXIST)
                {
                    itr->second->failed = true;
                }
                itr->second->id = backupcounter++;
            }
        }
    }
    delete megaCmdListener;
    return false;
}
#endif

void MegaCmdExecuter::executecommand(vector<string> words, map<string, int> *clflags, map<string, string> *cloptions)
{
    MegaNode* n = NULL;
    if (words[0] == "ls")
    {
        if (!api->isFilesystemAvailable())
        {
            setCurrentOutCode(MCMD_NOTLOGGEDIN);
            LOG_err << "Not logged in.";
            return;
        }
        int recursive = getFlag(clflags, "R") + getFlag(clflags, "r");
        int extended_info = getFlag(clflags, "a");
        int show_versions = getFlag(clflags, "versions");
        bool summary = getFlag(clflags, "l");
        bool firstprint = true;
        bool humanreadable = getFlag(clflags, "h");

        if ((int)words.size() > 1)
        {
            unescapeifRequired(words[1]);

            string rNpath = "NULL";
            if (words[1].find('/') != string::npos)
            {
                string cwpath = getCurrentPath();
                if (words[1].find(cwpath) == string::npos)
                {
                    rNpath = "";
                }
                else
                {
                    rNpath = cwpath;
                }
            }

            if (isRegExp(words[1]))
            {
                vector<string> *pathsToList = nodesPathsbypath(words[1].c_str(), getFlag(clflags,"use-pcre"));
                if (pathsToList && pathsToList->size())
                {
                    for (std::vector< string >::iterator it = pathsToList->begin(); it != pathsToList->end(); ++it)
                    {
                        string nodepath= *it;
                        MegaNode *ncwd = api->getNodeByHandle(cwd);
                        if (ncwd)
                        {
                            MegaNode * n = nodebypath(nodepath.c_str());
                            if (n)
                            {
                                if (!n->getType() == MegaNode::TYPE_FILE)
                                {
                                    OUTSTREAM << nodepath << ": " << std::endl;
                                }
                                if (summary)
                                {
                                    if (firstprint)
                                    {
                                        dumpNodeSummaryHeader();
                                        firstprint = false;
                                    }
                                    dumpTreeSummary(n, recursive, show_versions, 0, humanreadable, rNpath);
                                }
                                else
                                {
                                    dumptree(n, recursive, extended_info, show_versions, 0, rNpath);
                                }
                                if (( !n->getType() == MegaNode::TYPE_FILE ) && (( it + 1 ) != pathsToList->end()))
                                {
                                    OUTSTREAM << std::endl;
                                }
                                delete n;
                            }
                            else
                            {
                                LOG_debug << "Unexpected: matching path has no associated node: " << nodepath << ". Could have been deleted in the process";
                            }
                            delete ncwd;
                        }
                        else
                        {
                            setCurrentOutCode(MCMD_INVALIDSTATE);
                            LOG_err << "Couldn't find woking folder (it might been deleted)";
                        }
                    }
                    pathsToList->clear();
                    delete pathsToList;
                }
                else
                {
                    setCurrentOutCode(MCMD_NOTFOUND);
                    LOG_err << "Couldn't find \"" << words[1] << "\"";
                }
            }
            else
            {
                n = nodebypath(words[1].c_str());
                if (n)
                {
                    if (summary)
                    {
                        if (firstprint)
                        {
                            dumpNodeSummaryHeader();
                            firstprint = false;
                        }
                        dumpTreeSummary(n, recursive, show_versions, 0, humanreadable, rNpath);
                    }
                    else
                    {
                        dumptree(n, recursive, extended_info, show_versions, 0, rNpath);
                    }
                    delete n;
                }
                else
                {
                    setCurrentOutCode(MCMD_NOTFOUND);
                    LOG_err << "Couldn't find " << words[1];
                }
            }
        }
        else
        {
            n = api->getNodeByHandle(cwd);
            if (n)
            {
                if (summary)
                {
                    if (firstprint)
                    {
                        dumpNodeSummaryHeader();
                        firstprint = false;
                    }
                    dumpTreeSummary(n, recursive, show_versions, 0, humanreadable);
                }
                else
                {
                    dumptree(n, recursive, extended_info, show_versions);
                }
                delete n;
            }
        }
        return;
    }
    else if (words[0] == "find")
    {
        string pattern = getOption(cloptions, "pattern", "*");
        int printfileinfo = getFlag(clflags,"l");

        if (!api->isFilesystemAvailable())
        {
            setCurrentOutCode(MCMD_NOTLOGGEDIN);
            LOG_err << "Not logged in.";
            return;
        }

        time_t minTime = -1;
        time_t maxTime = -1;
        string mtimestring = getOption(cloptions, "mtime", "");
        if ("" != mtimestring && !getMinAndMaxTime(mtimestring, &minTime, &maxTime))
        {
            setCurrentOutCode(MCMD_EARGS);
            LOG_err << "Invalid time " << mtimestring;
            return;
        }

        int64_t minSize = -1;
        int64_t maxSize = -1;
        string sizestring = getOption(cloptions, "size", "");
        if ("" != sizestring && !getMinAndMaxSize(sizestring, &minSize, &maxSize))
        {
            setCurrentOutCode(MCMD_EARGS);
            LOG_err << "Invalid time " << sizestring;
            return;
        }


        if (words.size() <= 1)
        {
            n = api->getNodeByHandle(cwd);
            doFind(n, "", printfileinfo, pattern, getFlag(clflags,"use-pcre"), minTime, maxTime, minSize, maxSize);
            delete n;
        }
        for (int i = 1; i < (int)words.size(); i++)
        {
            if (isRegExp(words[i]))
            {
                vector<MegaNode *> *nodesToFind = nodesbypath(words[i].c_str(), getFlag(clflags,"use-pcre"));
                if (nodesToFind->size())
                {
                    for (std::vector< MegaNode * >::iterator it = nodesToFind->begin(); it != nodesToFind->end(); ++it)
                    {
                        MegaNode * nodeToFind = *it;
                        if (nodeToFind)
                        {
                            doFind(nodeToFind, words[i], printfileinfo, pattern, getFlag(clflags,"use-pcre"), minTime, maxTime, minSize, maxSize);
                            delete nodeToFind;
                        }
                    }
                    nodesToFind->clear();
                }
                else
                {
                    setCurrentOutCode(MCMD_NOTFOUND);
                    LOG_err << words[i] << ": No such file or directory";
                }
                delete nodesToFind;
            }
            else
            {
                n = nodebypath(words[i].c_str());
                if (!n)
                {
                    setCurrentOutCode(MCMD_NOTFOUND);
                    LOG_err << "Couldn't find " << words[i];
                }
                else
                {
                    doFind(n, words[i], printfileinfo, pattern, getFlag(clflags,"use-pcre"), minTime, maxTime, minSize, maxSize);
                    delete n;
                }
            }

        }
    }
    else if (words[0] == "cd")
    {
        if (!api->isFilesystemAvailable())
        {
            setCurrentOutCode(MCMD_NOTLOGGEDIN);
            LOG_err << "Not logged in.";
            return;
        }
        if (words.size() > 1)
        {
            if (( n = nodebypath(words[1].c_str())))
            {
                if (n->getType() == MegaNode::TYPE_FILE)
                {
                    setCurrentOutCode(MCMD_NOTFOUND);
                    LOG_err << words[1] << ": Not a directory";
                }
                else
                {
                    cwd = n->getHandle();

                    updateprompt(api, cwd);
                }
                delete n;
            }
            else
            {
                setCurrentOutCode(MCMD_NOTFOUND);
                LOG_err << words[1] << ": No such file or directory";
            }
        }
        else
        {
            MegaNode * rootNode = api->getRootNode();
            if (!rootNode)
            {
                LOG_err << "nodes not fetched";
                setCurrentOutCode(MCMD_NOFETCH);
                delete rootNode;
                return;
            }
            cwd = rootNode->getHandle();
            updateprompt(api, cwd);

            delete rootNode;
        }

        return;
    }
    else if (words[0] == "rm")
    {
        if (!api->isFilesystemAvailable())
        {
            setCurrentOutCode(MCMD_NOTLOGGEDIN);
            LOG_err << "Not logged in.";
            return;
        }
        if (words.size() > 1)
        {
            if (interactiveThread() && nodesToConfirmDelete.size())
            {
                //clear all previous nodes to confirm delete (could have been not cleared in case of ctrl+c)
                for (std::vector< MegaNode * >::iterator it = nodesToConfirmDelete.begin(); it != nodesToConfirmDelete.end(); ++it)
                {
                    delete *it;
                }
                nodesToConfirmDelete.clear();
            }

            bool force = getFlag(clflags, "f");
            bool none = false;

            for (unsigned int i = 1; i < words.size(); i++)
            {
                unescapeifRequired(words[i]);
                if (isRegExp(words[i]))
                {
                    vector<MegaNode *> *nodesToDelete = nodesbypath(words[i].c_str(), getFlag(clflags,"use-pcre"));
                    if (nodesToDelete->size())
                    {
                        for (std::vector< MegaNode * >::iterator it = nodesToDelete->begin(); !none && it != nodesToDelete->end(); ++it)
                        {
                            MegaNode * nodeToDelete = *it;
                            if (nodeToDelete)
                            {
                                int confirmationCode = deleteNode(nodeToDelete, api, getFlag(clflags, "r"), force);
                                if (confirmationCode == MCMDCONFIRM_ALL)
                                {
                                    force = true;
                                }
                                else if (confirmationCode == MCMDCONFIRM_NONE)
                                {
                                    none = true;
                                }

                            }
                        }
                        nodesToDelete->clear();
                    }
                    else
                    {
                        setCurrentOutCode(MCMD_NOTFOUND);
                        LOG_err << words[i] << ": No such file or directory";
                    }
                    delete nodesToDelete;
                }
                else if (!none)
                {
                    MegaNode * nodeToDelete = nodebypath(words[i].c_str());
                    if (nodeToDelete)
                    {
                        int confirmationCode = deleteNode(nodeToDelete, api, getFlag(clflags, "r"), force);
                        if (confirmationCode == MCMDCONFIRM_ALL)
                        {
                            force = true;
                        }
                        else if (confirmationCode == MCMDCONFIRM_NONE)
                        {
                            none = true;
                        }
                    }
                    else
                    {
                        setCurrentOutCode(MCMD_NOTFOUND);
                        LOG_err << words[i] << ": No such file or directory";
                    }
                }
            }
        }
        else
        {
            setCurrentOutCode(MCMD_EARGS);
            LOG_err << "      " << getUsageStr("rm");
        }

        return;
    }
    else if (words[0] == "mv")
    {
        if (!api->isFilesystemAvailable())
        {
            setCurrentOutCode(MCMD_NOTLOGGEDIN);
            LOG_err << "Not logged in.";
            return;
        }
        if (words.size() > 2)
        {
            string destiny = words[words.size()-1];

            if (words.size() > 3 && !isValidFolder(destiny))
            {
                setCurrentOutCode(MCMD_INVALIDTYPE);
                LOG_err << destiny << " must be a valid folder";
                return;
            }

            for (unsigned int i=1;i<(words.size()-1);i++)
            {
                string source = words[i];

                if (isRegExp(source))
                {
                    vector<MegaNode *> *nodesToList = nodesbypath(words[i].c_str(), getFlag(clflags,"use-pcre"));
                    if (nodesToList)
                    {
                        if (!nodesToList->size())
                        {
                            setCurrentOutCode(MCMD_NOTFOUND);
                            LOG_err << source << ": No such file or directory";
                        }

                        bool destinyisok=true;
                        if (nodesToList->size() > 1 && !isValidFolder(destiny))
                        {
                            destinyisok = false;
                            setCurrentOutCode(MCMD_INVALIDTYPE);
                            LOG_err << destiny << " must be a valid folder";
                        }

                        if (destinyisok)
                        {
                            for (std::vector< MegaNode * >::iterator it = nodesToList->begin(); it != nodesToList->end(); ++it)
                            {
                                MegaNode * n = *it;
                                if (n)
                                {
                                    move(n, destiny);
                                    delete n;
                                }
                            }
                        }

                        nodesToList->clear();
                        delete nodesToList;
                    }
                }
                else
                {
                    if (( n = nodebypath(source.c_str())) )
                    {
                        move(n, destiny);
                        delete n;
                    }
                    else
                    {
                        setCurrentOutCode(MCMD_NOTFOUND);
                        LOG_err << source << ": No such file or directory";
                    }
                }
            }

        }
        else
        {
            setCurrentOutCode(MCMD_EARGS);
            LOG_err << "      " << getUsageStr("mv");
        }

        return;
    }
    else if (words[0] == "cp")
    {
        if (!api->isFilesystemAvailable())
        {
            setCurrentOutCode(MCMD_NOTLOGGEDIN);
            LOG_err << "Not logged in.";
            return;
        }
        MegaNode* tn;
        string targetuser;
        string newname;

        if (words.size() > 2)
        {
            if (( n = nodebypath(words[1].c_str())))
            {
                if (( tn = nodebypath(words[2].c_str(), &targetuser, &newname)))
                {
                    if (tn->getHandle() == n->getHandle())
                    {
                        LOG_err << "Source and destiny are the same";
                    }
                    else
                    {
                        if (newname.size()) //target not found, but tn has what was before the last "/" in the path.
                        {
                            if (n->getType() == MegaNode::TYPE_FILE)
                            {
                                //copy with new name
                                MegaCmdListener *megaCmdListener = new MegaCmdListener(NULL);
                                api->copyNode(n, tn, newname.c_str(), megaCmdListener); //only works for files
                                megaCmdListener->wait();
                                checkNoErrors(megaCmdListener->getError(), "copy node");
                                delete megaCmdListener;
                            }
                            else //copy & rename
                            {
                                //copy with new name
                                MegaCmdListener *megaCmdListener = new MegaCmdListener(NULL);
                                api->copyNode(n, tn, megaCmdListener);
                                megaCmdListener->wait();
                                if (checkNoErrors(megaCmdListener->getError(), "copy node"))
                                {
                                    MegaNode * newNode = api->getNodeByHandle(megaCmdListener->getRequest()->getNodeHandle());
                                    if (newNode)
                                    {
                                        MegaCmdListener *megaCmdListener = new MegaCmdListener(NULL);
                                        api->renameNode(newNode, newname.c_str(), megaCmdListener);
                                        megaCmdListener->wait();
                                        checkNoErrors(megaCmdListener->getError(), "rename new node");
                                        delete megaCmdListener;
                                        delete newNode;
                                    }
                                    else
                                    {
                                        LOG_err << " Couldn't find new node created upon cp";
                                    }
                                }
                                delete megaCmdListener;
                            }
                        }
                        else
                        { //target exists
                            if (tn->getType() == MegaNode::TYPE_FILE)
                            {
                                if (n->getType() == MegaNode::TYPE_FILE)
                                {
                                    // overwrite target if source and target are files
                                    MegaNode *tnParentNode = api->getNodeByHandle(tn->getParentHandle());
                                    if (tnParentNode) // (there should never be any orphaned filenodes)
                                    {
                                        const char* name_to_replace = tn->getName();
                                        //copy with new name
                                        MegaCmdListener *megaCmdListener = new MegaCmdListener(NULL);
                                        api->copyNode(n, tnParentNode, name_to_replace, megaCmdListener);
                                        megaCmdListener->wait();
                                        delete megaCmdListener;
                                        delete tnParentNode;

                                        //remove target node
                                        megaCmdListener = new MegaCmdListener(NULL);
                                        api->remove(tn, megaCmdListener);
                                        megaCmdListener->wait();
                                        checkNoErrors(megaCmdListener->getError(), "delete target node");
                                        delete megaCmdListener;
                                    }
                                    else
                                    {
                                        setCurrentOutCode(MCMD_INVALIDSTATE);
                                        LOG_fatal << "Destiny node is orphan!!!";
                                    }
                                }
                                else
                                {
                                    setCurrentOutCode(MCMD_INVALIDTYPE);
                                    LOG_err << "Cannot overwrite file with folder";
                                    return;
                                }
                            }
                            else //copying into folder
                            {
                                MegaCmdListener *megaCmdListener = new MegaCmdListener(NULL);
                                api->copyNode(n, tn, megaCmdListener);
                                megaCmdListener->wait();
                                checkNoErrors(megaCmdListener->getError(), "copy node");
                                delete megaCmdListener;
                            }
                        }
                    }
                    delete tn;
                }
                else if (targetuser.size())
                {
                    MegaCmdListener *megaCmdListener = new MegaCmdListener(NULL);
                    api->sendFileToUser(n,targetuser.c_str(),megaCmdListener);
                    megaCmdListener->wait();
                    checkNoErrors(megaCmdListener->getError(), "send file to user");
                    delete megaCmdListener;
                }
                else
                {
                    setCurrentOutCode(MCMD_NOTFOUND);
                    LOG_err << words[2] << " Couldn't find destination";
                }
                delete n;
            }
            else
            {
                setCurrentOutCode(MCMD_NOTFOUND);
                LOG_err << words[1] << ": No such file or directory";
            }
        }
        else
        {
            setCurrentOutCode(MCMD_EARGS);
            LOG_err << "      " << getUsageStr("cp");
        }

        return;
    }
    else if (words[0] == "du")
    {
        if (!api->isFilesystemAvailable())
        {
            setCurrentOutCode(MCMD_NOTLOGGEDIN);
            LOG_err << "Not logged in.";
            return;
        }
        long long totalSize = 0;
        long long currentSize = 0;
        long long totalVersionsSize = 0;
        string dpath;
        if (words.size() == 1)
        {
            words.push_back(".");
        }

        bool humanreadable = getFlag(clflags, "h");
        bool show_versions_size = getFlag(clflags, "versions");
        bool firstone = true;

        for (unsigned int i = 1; i < words.size(); i++)
        {
            unescapeifRequired(words[i]);
            if (isRegExp(words[i]))
            {
                vector<MegaNode *> *nodesToList = nodesbypath(words[i].c_str(), getFlag(clflags,"use-pcre"));
                if (nodesToList)
                {
                    for (std::vector< MegaNode * >::iterator it = nodesToList->begin(); it != nodesToList->end(); ++it)
                    {
                        MegaNode * n = *it;
                        if (n)
                        {
                            if (firstone)//print header
                            {
                                OUTSTREAM << getFixLengthString("FILENAME",40) << getFixLengthString("SIZE", 12, ' ', true);
                                if (show_versions_size)
                                {
                                    OUTSTREAM << getFixLengthString("S.WITH VERS", 12, ' ', true);;
                                }
                                OUTSTREAM << std::endl;
                                firstone = false;
                            }
                            currentSize = api->getSize(n);
                            totalSize += currentSize;

                            dpath = getDisplayPath(words[i], n);
                            OUTSTREAM << getFixLengthString(dpath+":",40) << getFixLengthString(sizeToText(currentSize, true, humanreadable), 12, ' ', true);
                            if (show_versions_size)
                            {
                                long long sizeWithVersions = getVersionsSize(n);
                                OUTSTREAM << getFixLengthString(sizeToText(sizeWithVersions, true, humanreadable), 12, ' ', true);
                                totalVersionsSize += sizeWithVersions;
                            }

                            OUTSTREAM << std::endl;
                            delete n;
                        }
                    }

                    nodesToList->clear();
                    delete nodesToList;
                }
            }
            else
            {
                if (!( n = nodebypath(words[i].c_str())))
                {
                    setCurrentOutCode(MCMD_NOTFOUND);
                    LOG_err << words[i] << ": No such file or directory";
                    return;
                }

                currentSize = api->getSize(n);
                totalSize += currentSize;
                dpath = getDisplayPath(words[i], n);
                if (dpath.size())
                {
                    if (firstone)//print header
                    {
                        OUTSTREAM << getFixLengthString("FILENAME",40) << getFixLengthString("SIZE", 12, ' ', true);
                        if (show_versions_size)
                        {
                            OUTSTREAM << getFixLengthString("S.WITH VERS", 12, ' ', true);;
                        }
                        OUTSTREAM << std::endl;
                        firstone = false;
                    }

                    OUTSTREAM << getFixLengthString(dpath+":",40) << getFixLengthString(sizeToText(currentSize, true, humanreadable), 12, ' ', true);
                    if (show_versions_size)
                    {
                        long long sizeWithVersions = getVersionsSize(n);
                        OUTSTREAM << getFixLengthString(sizeToText(sizeWithVersions, true, humanreadable), 12, ' ', true);
                        totalVersionsSize += sizeWithVersions;
                    }
                    OUTSTREAM << std::endl;

                }
                delete n;
            }
        }

        if (!firstone)
        {
            OUTSTREAM << "----------------------------------------------------------------" << std::endl;

            OUTSTREAM << getFixLengthString("Total storage used:",40) << getFixLengthString(sizeToText(totalSize, true, humanreadable), 12, ' ', true);
            //OUTSTREAM << "Total storage used: " << std::setw(22) << sizeToText(totalSize, true, humanreadable);
            if (show_versions_size)
            {
                OUTSTREAM << getFixLengthString(sizeToText(totalVersionsSize, true, humanreadable), 12, ' ', true);
            }
            OUTSTREAM << std::endl;
        }
        return;
    }
    else if (words[0] == "get")
    {
        int clientID = getintOption(cloptions, "clientID", -1);
        if (words.size() > 1 && words.size() < 4)
        {
            string path = "./";
            bool background = getFlag(clflags,"q");
            if (background)
            {
                clientID = -1;
            }

            MegaCmdMultiTransferListener *megaCmdMultiTransferListener = new MegaCmdMultiTransferListener(api, sandboxCMD, NULL, clientID);

            bool ignorequotawarn = getFlag(clflags,"ignore-quota-warn");
            bool destinyIsFolder = false;
            if (isPublicLink(words[1]))
            {
                if (getLinkType(words[1]) == MegaNode::TYPE_FILE)
                {
                    if (words.size() > 2)
                    {
                        path = words[2];
                        destinyIsFolder = IsFolder(path);
                        if (destinyIsFolder)
                        {
                            if (! (path.find_last_of("/") == path.size()-1) && ! (path.find_last_of("\\") == path.size()-1))
                            {
#ifdef _WIN32
                                path+="\\";
#else
                                path+="/";
#endif
                            }
                            if (!canWrite(path))
                            {
                                setCurrentOutCode(MCMD_NOTPERMITTED);
                                LOG_err << "Write not allowed in " << path;
                                delete megaCmdMultiTransferListener;
                                return;
                            }
                        }
                        else
                        {
                            if (!TestCanWriteOnContainingFolder(&path))
                            {
                                delete megaCmdMultiTransferListener;
                                return;
                            }
                        }
                    }
                    MegaCmdListener *megaCmdListener = new MegaCmdListener(NULL);
                    api->getPublicNode(words[1].c_str(), megaCmdListener);
                    megaCmdListener->wait();

                    if (!megaCmdListener->getError())
                    {
                        LOG_fatal << "No error in listener at get public node";
                    }
                    else if (!checkNoErrors(megaCmdListener->getError(), "get public node"))
                    {
                        if (megaCmdListener->getError()->getErrorCode() == MegaError::API_EARGS)
                        {
                            LOG_err << "The link provided might be incorrect: " << words[1].c_str();
                        }
                        else if (megaCmdListener->getError()->getErrorCode() == MegaError::API_EINCOMPLETE)
                        {
                            LOG_err << "The key is missing or wrong " << words[1].c_str();
                        }
                    }
                    else
                    {
                        if (megaCmdListener->getRequest() && megaCmdListener->getRequest()->getFlag())
                        {
                            LOG_err << "Key not valid " << words[1].c_str();
                        }
                        if (megaCmdListener->getRequest())
                        {
                            if (destinyIsFolder && getFlag(clflags,"m"))
                            {
                                while( (path.find_last_of("/") == path.size()-1) || (path.find_last_of("\\") == path.size()-1))
                                {
                                    path=path.substr(0,path.size()-1);
                                }
                            }
                            MegaNode *n = megaCmdListener->getRequest()->getPublicMegaNode();
                            downloadNode(path, api, n, background, ignorequotawarn, clientID, megaCmdMultiTransferListener);
                            delete n;
                        }
                        else
                        {
                            LOG_err << "Empty Request at get";
                        }
                    }
                    delete megaCmdListener;
                }
                else if (getLinkType(words[1]) == MegaNode::TYPE_FOLDER)
                {
                    if (words.size() > 2)
                    {
                        path = words[2];
                        destinyIsFolder = IsFolder(path);
                        if (destinyIsFolder)
                        {
                            if (! (path.find_last_of("/") == path.size()-1) && ! (path.find_last_of("\\") == path.size()-1))
                            {
#ifdef _WIN32
                                path+="\\";
#else
                                path+="/";
#endif
                            }
                            if (!canWrite(words[2]))
                            {
                                setCurrentOutCode(MCMD_NOTPERMITTED);
                                LOG_err << "Write not allowed in " << words[2];
                                delete megaCmdMultiTransferListener;
                                return;
                            }
                        }
                        else
                        {
                            setCurrentOutCode(MCMD_INVALIDTYPE);
                            LOG_err << words[2] << " is not a valid Download Folder";
                            delete megaCmdMultiTransferListener;
                            return;
                        }
                    }

                    MegaApi* apiFolder = getFreeApiFolder();
                    char *accountAuth = api->getAccountAuth();
                    apiFolder->setAccountAuth(accountAuth);
                    delete []accountAuth;

                    MegaCmdListener *megaCmdListener = new MegaCmdListener(apiFolder, NULL);
                    apiFolder->loginToFolder(words[1].c_str(), megaCmdListener);
                    megaCmdListener->wait();
                    if (checkNoErrors(megaCmdListener->getError(), "login to folder"))
                    {
                        MegaCmdListener *megaCmdListener2 = new MegaCmdListener(apiFolder, NULL);
                        apiFolder->fetchNodes(megaCmdListener2);
                        megaCmdListener2->wait();
                        if (checkNoErrors(megaCmdListener2->getError(), "access folder link " + words[1]))
                        {
                            MegaNode *folderRootNode = apiFolder->getRootNode();
                            if (folderRootNode)
                            {
                                if (destinyIsFolder && getFlag(clflags,"m"))
                                {
                                    while( (path.find_last_of("/") == path.size()-1) || (path.find_last_of("\\") == path.size()-1))
                                    {
                                        path=path.substr(0,path.size()-1);
                                    }
                                }
                                MegaNode *authorizedNode = apiFolder->authorizeNode(folderRootNode);
                                if (authorizedNode != NULL)
                                {
                                    downloadNode(path, api, authorizedNode, background, ignorequotawarn, clientID, megaCmdMultiTransferListener);
                                    delete authorizedNode;
                                }
                                else
                                {
                                    LOG_debug << "Node couldn't be authorized: " << words[1] << ". Downloading as non-loged user";
                                    downloadNode(path, apiFolder, folderRootNode, background, ignorequotawarn, clientID, megaCmdMultiTransferListener);
                                }
                                delete folderRootNode;
                            }
                            else
                            {
                                setCurrentOutCode(MCMD_INVALIDSTATE);
                                LOG_err << "Couldn't get root folder for folder link";
                            }
                        }
                        delete megaCmdListener2;
                    }
                    delete megaCmdListener;
                    freeApiFolder(apiFolder);
                }
                else
                {
                    setCurrentOutCode(MCMD_INVALIDTYPE);
                    LOG_err << "Invalid link: " << words[1];
                }
            }
            else //remote file
            {
                if (!api->isFilesystemAvailable())
                {
                    setCurrentOutCode(MCMD_NOTLOGGEDIN);
                    LOG_err << "Not logged in.";
                    return;
                }
                unescapeifRequired(words[1]);

                if (isRegExp(words[1]))
                {
                    vector<MegaNode *> *nodesToGet = nodesbypath(words[1].c_str(), getFlag(clflags,"use-pcre"));
                    if (nodesToGet)
                    {
                        if (words.size() > 2)
                        {
                            path = words[2];
                            destinyIsFolder = IsFolder(path);
                            if (destinyIsFolder)
                            {
                                if (! (path.find_last_of("/") == path.size()-1) && ! (path.find_last_of("\\") == path.size()-1))
                                {
#ifdef _WIN32
                                    path+="\\";
#else
                                    path+="/";
#endif
                                }
                                if (!canWrite(words[2]))
                                {
                                    setCurrentOutCode(MCMD_NOTPERMITTED);
                                    LOG_err << "Write not allowed in " << words[2];
                                    for (std::vector< MegaNode * >::iterator it = nodesToGet->begin(); it != nodesToGet->end(); ++it)
                                    {
                                        delete (MegaNode *)*it;
                                    }
                                    delete nodesToGet;
                                    delete megaCmdMultiTransferListener;
                                    return;
                                }
                            }
                            else if (nodesToGet->size()>1) //several files into one file!
                            {
                                setCurrentOutCode(MCMD_INVALIDTYPE);
                                LOG_err << words[2] << " is not a valid Download Folder";
                                for (std::vector< MegaNode * >::iterator it = nodesToGet->begin(); it != nodesToGet->end(); ++it)
                                {
                                    delete (MegaNode *)*it;
                                }
                                delete nodesToGet;
                                delete megaCmdMultiTransferListener;
                                return;
                            }
                            else //destiny non existing or a file
                            {
                                if (!TestCanWriteOnContainingFolder(&path))
                                {
                                    for (std::vector< MegaNode * >::iterator it = nodesToGet->begin(); it != nodesToGet->end(); ++it)
                                    {
                                        delete (MegaNode *)*it;
                                    }
                                    delete nodesToGet;
                                    delete megaCmdMultiTransferListener;
                                    return;
                                }
                            }
                        }
                        if (destinyIsFolder && getFlag(clflags,"m"))
                        {
                            while( (path.find_last_of("/") == path.size()-1) || (path.find_last_of("\\") == path.size()-1))
                            {
                                path=path.substr(0,path.size()-1);
                            }
                        }
                        for (std::vector< MegaNode * >::iterator it = nodesToGet->begin(); it != nodesToGet->end(); ++it)
                        {
                            MegaNode * n = *it;
                            if (n)
                            {
                                downloadNode(path, api, n, background, ignorequotawarn, clientID, megaCmdMultiTransferListener);
                                delete n;
                            }
                        }
                        if (!nodesToGet->size())
                        {
                            setCurrentOutCode(MCMD_NOTFOUND);
                            LOG_err << "Couldn't find " << words[1];
                        }

                        nodesToGet->clear();
                        delete nodesToGet;
                    }
                }
                else //not regexp
                {
                    MegaNode *n = nodebypath(words[1].c_str());
                    if (n)
                    {
                        if (words.size() > 2)
                        {
                            if (n->getType() == MegaNode::TYPE_FILE)
                            {
                                path = words[2];
                                destinyIsFolder = IsFolder(path);
                                if (destinyIsFolder)
                                {
                                    if (! (path.find_last_of("/") == path.size()-1) && ! (path.find_last_of("\\") == path.size()-1))
                                    {
#ifdef _WIN32
                                        path+="\\";
#else
                                        path+="/";
#endif
                                    }
                                    if (!canWrite(words[2]))
                                    {
                                        setCurrentOutCode(MCMD_NOTPERMITTED);
                                        LOG_err << "Write not allowed in " << words[2];
                                        delete megaCmdMultiTransferListener;
                                        return;
                                    }
                                }
                                else
                                {
                                    if (!TestCanWriteOnContainingFolder(&path))
                                    {
                                        delete megaCmdMultiTransferListener;
                                        return;
                                    }
                                }
                            }
                            else
                            {
                                path = words[2];
                                destinyIsFolder = IsFolder(path);
                                if (destinyIsFolder)
                                {
                                    if (! (path.find_last_of("/") == path.size()-1) && ! (path.find_last_of("\\") == path.size()-1))
                                    {
#ifdef _WIN32
                                        path+="\\";
#else
                                        path+="/";
#endif
                                    }
                                    if (!canWrite(words[2]))
                                    {
                                        setCurrentOutCode(MCMD_NOTPERMITTED);
                                        LOG_err << "Write not allowed in " << words[2];
                                        delete megaCmdMultiTransferListener;
                                        return;
                                    }
                                }
                                else
                                {
                                    setCurrentOutCode(MCMD_INVALIDTYPE);
                                    LOG_err << words[2] << " is not a valid Download Folder";
                                    delete megaCmdMultiTransferListener;
                                    return;
                                }
                            }
                        }
                        if (destinyIsFolder && getFlag(clflags,"m"))
                        {
                            while( (path.find_last_of("/") == path.size()-1) || (path.find_last_of("\\") == path.size()-1))
                            {
                                path=path.substr(0,path.size()-1);
                            }
                        }
                        downloadNode(path, api, n, background, ignorequotawarn, clientID, megaCmdMultiTransferListener);
                        delete n;
                    }
                    else
                    {
                        setCurrentOutCode(MCMD_NOTFOUND);
                        LOG_err << "Couldn't find file";
                    }
                }
            }

            megaCmdMultiTransferListener->waitMultiEnd();
            if (megaCmdMultiTransferListener->getFinalerror() != MegaError::API_OK)
            {
                setCurrentOutCode(megaCmdMultiTransferListener->getFinalerror());
                LOG_err << "Download failed. error code:" << MegaError::getErrorString(megaCmdMultiTransferListener->getFinalerror());
            }

            informProgressUpdate(PROGRESS_COMPLETE, megaCmdMultiTransferListener->getTotalbytes(), clientID);
            delete megaCmdMultiTransferListener;
        }
        else
        {
            setCurrentOutCode(MCMD_EARGS);
            LOG_err << "      " << getUsageStr("get");
        }

        return;
    }
#ifdef ENABLE_BACKUPS

    else if (words[0] == "backup")
    {
        bool dodelete = getFlag(clflags,"d");
        bool abort = getFlag(clflags,"a");
        bool listinfo = getFlag(clflags,"l");
        bool listhistory = getFlag(clflags,"h");

//        //TODO: do the following functionality
//        bool stop = getFlag(clflags,"s");
//        bool resume = getFlag(clflags,"r");
//        bool initiatenow = getFlag(clflags,"i");

        int PATHSIZE = getintOption(cloptions,"path-display-size");
        if (!PATHSIZE)
        {
            // get screen size for output purposes
            unsigned int width = getNumberOfCols(75);
            PATHSIZE = std::min(60,int((width-46)/2));
        }

        bool firstbackup = true;
        string speriod=getOption(cloptions, "period");
        int64_t numBackups = getintOption(cloptions, "num-backups", -1);

        if (words.size() == 3)
        {
            string local = words.at(1);
            string remote = words.at(2);

            createOrModifyBackup(local, remote, speriod, numBackups);
        }
        else if (words.size() == 2)
        {
            string local = words.at(1);

            MegaBackup *backup = api->getBackupByPath(local.c_str());
            if (!backup)
            {
                backup = api->getBackupByTag(toInteger(local, -1));
            }
            map<string, backup_struct *>::iterator itr;
            if (backup)
            {
                int backupid = -1;
                for ( itr = ConfigurationManager::configuredBackups.begin(); itr != ConfigurationManager::configuredBackups.end(); itr++ )
                {
                    if (itr->second->tag == backup->getTag())
                    {
                        backupid = itr->second->id;
                        break;
                    }
                }
                if (backupid == -1)
                {
                    LOG_err << " Requesting info of unregistered backup: " << local;
                }

                if (dodelete)
                {
                    MegaCmdListener *megaCmdListener = new MegaCmdListener(api, NULL);
                    api->removeBackup(backup->getTag(), megaCmdListener);
                    megaCmdListener->wait();
                    if (checkNoErrors(megaCmdListener->getError(), "remove backup"))
                    {
                        if (backupid != -1)
                        {
                          ConfigurationManager::configuredBackups.erase(itr);
                        }
                        mtxBackupsMap.lock();
                        ConfigurationManager::saveBackups(&ConfigurationManager::configuredBackups);
                        mtxBackupsMap.unlock();
                        OUTSTREAM << " Backup removed succesffuly: " << local << std::endl;
                    }
                }
                else if (abort)
                {
                    MegaCmdListener *megaCmdListener = new MegaCmdListener(api, NULL);
                    api->abortCurrentBackup(backup->getTag(), megaCmdListener);
                    megaCmdListener->wait();
                    if (checkNoErrors(megaCmdListener->getError(), "abort backup"))
                    {
                        OUTSTREAM << " Backup aborted succesffuly: " << local << std::endl;
                    }
                }
                else
                {
                    if (speriod.size() || numBackups != -1)
                    {
                        createOrModifyBackup(backup->getLocalFolder(), "", speriod, numBackups);
                    }
                    else
                    {
                        if(firstbackup)
                        {
                            printBackupHeader(PATHSIZE);
                            firstbackup = false;
                        }

                        printBackup(backup->getTag(), backup, PATHSIZE, listinfo, listhistory);
                    }
                }
                delete backup;
            }
            else
            {
                setCurrentOutCode(MCMD_NOTFOUND);
                LOG_err << "Backup not found: " << local;
            }
        }
        else if (words.size() == 1) //list backups
        {
            mtxBackupsMap.lock();
            for (map<string, backup_struct *>::iterator itr = ConfigurationManager::configuredBackups.begin(); itr != ConfigurationManager::configuredBackups.end(); itr++ )
            {
                if(firstbackup)
                {
                    printBackupHeader(PATHSIZE);
                    firstbackup = false;
                }
                printBackup(itr->second, PATHSIZE, listinfo, listhistory);
            }
            if (!ConfigurationManager::configuredBackups.size())
            {
                setCurrentOutCode(MCMD_NOTFOUND);
                OUTSTREAM << "No backup configured. " << std::endl << " Usage: " << getUsageStr("backup") << std::endl;
            }
            mtxBackupsMap.unlock();

        }
        else
        {
            setCurrentOutCode(MCMD_EARGS);
            LOG_err << "      " << getUsageStr("backup");
        }
    }
#endif
    else if (words[0] == "put")
    {
        int clientID = getintOption(cloptions, "clientID", -1);

        if (!api->isFilesystemAvailable())
        {
            setCurrentOutCode(MCMD_NOTLOGGEDIN);
            LOG_err << "Not logged in.";
            return;
        }

        bool background = getFlag(clflags,"q");
        if (background)
        {
            clientID = -1;
        }

        MegaCmdMultiTransferListener *megaCmdMultiTransferListener = new MegaCmdMultiTransferListener(api, sandboxCMD, NULL, clientID);

        bool ignorequotawarn = getFlag(clflags,"ignore-quota-warn");

        if (words.size() > 1)
        {
            string targetuser;
            string newname = "";
            string destination = "";

            MegaNode *n = NULL;

            if (words.size() > 2)
            {
                destination = words[words.size() - 1];
                n = nodebypath(destination.c_str(), &targetuser, &newname);

                if (!n && getFlag(clflags,"c"))
                {
                    string destinationfolder(destination,0,destination.find_last_of("/"));
                    newname=string(destination,destination.find_last_of("/")+1,destination.size());
                    MegaNode *cwdNode = api->getNodeByHandle(cwd);
                    makedir(destinationfolder,true,cwdNode);
                    n = nodebypath(destinationfolder.c_str());
                    delete cwdNode;
                }
            }
            else
            {
                n = api->getNodeByHandle(cwd);
                words.push_back(".");
            }
            if (n)
            {
                if (n->getType() != MegaNode::TYPE_FILE)
                {
                    for (int i = 1; i < std::max(1, (int)words.size() - 1); i++)
                    {
                        if (words[i] == ".")
                        {
                            words[i] = getLPWD();
                        }
                        uploadNode(words[i], api, n, newname, background, ignorequotawarn, clientID, megaCmdMultiTransferListener);
                    }
                }
                else
                {
                    setCurrentOutCode(MCMD_INVALIDTYPE);
                    LOG_err << "Destination is not valid (expected folder or alike)";
                }
                delete n;


                megaCmdMultiTransferListener->waitMultiEnd();
                if (megaCmdMultiTransferListener->getFinalerror() != MegaError::API_OK)
                {
                    setCurrentOutCode(megaCmdMultiTransferListener->getFinalerror());
                    LOG_err << "Upload failed. error code:" << MegaError::getErrorString(megaCmdMultiTransferListener->getFinalerror());
                }

                informProgressUpdate(PROGRESS_COMPLETE, megaCmdMultiTransferListener->getTotalbytes(), clientID);
                delete megaCmdMultiTransferListener;
            }
            else
            {
                setCurrentOutCode(MCMD_NOTFOUND);
                LOG_err << "Couln't find destination folder: " << destination << ". Use -c to create folder structure";
            }
        }
        else
        {
            setCurrentOutCode(MCMD_EARGS);
            LOG_err << "      " << getUsageStr("put");
        }

        return;
    }
    else if (words[0] == "log")
    {
        if (words.size() == 1)
        {
            if (!getFlag(clflags, "s") && !getFlag(clflags, "c"))
            {
                OUTSTREAM << "CMD log level = " << getLogLevelStr(loggerCMD->getCmdLoggerLevel()) << std::endl;
                OUTSTREAM << "SDK log level = " << getLogLevelStr(loggerCMD->getApiLoggerLevel()) << std::endl;
            }
            else if (getFlag(clflags, "s"))
            {
                OUTSTREAM << "SDK log level = " << getLogLevelStr(loggerCMD->getApiLoggerLevel()) << std::endl;
            }
            else if (getFlag(clflags, "c"))
            {
                OUTSTREAM << "CMD log level = " << getLogLevelStr(loggerCMD->getCmdLoggerLevel()) << std::endl;
            }
        }
        else
        {
            int newLogLevel = getLogLevelNum(words[1].c_str());
            newLogLevel = std::max(newLogLevel, (int)MegaApi::LOG_LEVEL_FATAL);
            newLogLevel = std::min(newLogLevel, (int)MegaApi::LOG_LEVEL_MAX);
            if (!getFlag(clflags, "s") && !getFlag(clflags, "c"))
            {
                loggerCMD->setCmdLoggerLevel(newLogLevel);
                loggerCMD->setApiLoggerLevel(newLogLevel);
                OUTSTREAM << "CMD log level = " << getLogLevelStr(loggerCMD->getCmdLoggerLevel()) << std::endl;
                OUTSTREAM << "SDK log level = " << getLogLevelStr(loggerCMD->getApiLoggerLevel()) << std::endl;
            }
            else if (getFlag(clflags, "s"))
            {
                loggerCMD->setApiLoggerLevel(newLogLevel);
                OUTSTREAM << "SDK log level = " << getLogLevelStr(loggerCMD->getApiLoggerLevel()) << std::endl;
            }
            else if (getFlag(clflags, "c"))
            {
                loggerCMD->setCmdLoggerLevel(newLogLevel);
                OUTSTREAM << "CMD log level = " << getLogLevelStr(loggerCMD->getCmdLoggerLevel()) << std::endl;
            }
        }

        return;
    }
    else if (words[0] == "pwd")
    {
        if (!api->isFilesystemAvailable())
        {
            setCurrentOutCode(MCMD_NOTLOGGEDIN);
            LOG_err << "Not logged in.";
            return;
        }
        string cwpath = getCurrentPath();

        OUTSTREAM << cwpath << std::endl;

        return;
    }
    else if (words[0] == "lcd") //this only makes sense for interactive mode
    {
        if (words.size() > 1)
        {
            string localpath;
            fsAccessCMD->path2local(&words[1], &localpath);
            if (fsAccessCMD->chdirlocal(&localpath)) // maybe this is already checked in chdir
            {
                LOG_debug << "Local folder changed to: " << words[1];
            }
            else
            {
                setCurrentOutCode(MCMD_INVALIDTYPE);
                LOG_err << "Not a valid folder: " << words[1];
            }
        }
        else
        {
            setCurrentOutCode(MCMD_EARGS);
            LOG_err << "      " << getUsageStr("lcd");
        }

        return;
    }
    else if (words[0] == "lpwd")
    {
        string absolutePath = getLPWD();

        OUTSTREAM << absolutePath << std::endl;
        return;
    }
    else if (words[0] == "ipc")
    {
        if (!api->isFilesystemAvailable())
        {
            setCurrentOutCode(MCMD_NOTLOGGEDIN);
            LOG_err << "Not logged in.";
            return;
        }
        if (words.size() > 1)
        {
            int action;
            string saction;

            if (getFlag(clflags, "a"))
            {
                action = MegaContactRequest::REPLY_ACTION_ACCEPT;
                saction = "Accept";
            }
            else if (getFlag(clflags, "d"))
            {
                action = MegaContactRequest::REPLY_ACTION_DENY;
                saction = "Reject";
            }
            else if (getFlag(clflags, "i"))
            {
                action = MegaContactRequest::REPLY_ACTION_IGNORE;
                saction = "Ignore";
            }
            else
            {
                setCurrentOutCode(MCMD_EARGS);
                LOG_err << "      " << getUsageStr("ipc");
                return;
            }

            MegaContactRequest * cr;
            string shandle = words[1];
            handle thehandle = api->base64ToUserHandle(shandle.c_str());

            if (shandle.find('@') != string::npos)
            {
                cr = getPcrByContact(shandle);
            }
            else
            {
                cr = api->getContactRequestByHandle(thehandle);
            }
            if (cr)
            {
                MegaCmdListener *megaCmdListener = new MegaCmdListener(api, NULL);
                api->replyContactRequest(cr, action, megaCmdListener);
                megaCmdListener->wait();
                if (checkNoErrors(megaCmdListener->getError(), "reply ipc"))
                {
                    OUTSTREAM << saction << "ed invitation by " << cr->getSourceEmail() << std::endl;
                }
                delete megaCmdListener;
                delete cr;
            }
            else
            {
                setCurrentOutCode(MCMD_NOTFOUND);
                LOG_err << "Could not find invitation " << shandle;
            }
        }
        else
        {
            setCurrentOutCode(MCMD_EARGS);
            LOG_err << "      " << getUsageStr("ipc");
            return;
        }
        return;
    }
    else if (words[0] == "https")
    {
        if (words.size() > 1 && (words[1] == "on" || words[1] == "off"))
        {
            bool onlyhttps = words[1] == "on";
            MegaCmdListener *megaCmdListener = new MegaCmdListener(NULL);
            api->useHttpsOnly(onlyhttps,megaCmdListener);
            megaCmdListener->wait();
            if (checkNoErrors(megaCmdListener->getError(), "change https"))
            {
                OUTSTREAM << "File transfer now uses " << (api->usingHttpsOnly()?"HTTPS":"HTTP") << std::endl;
                ConfigurationManager::savePropertyValue("https", api->usingHttpsOnly());
            }
            delete megaCmdListener;
            return;
        }
        else if (words.size() > 1)
        {
            setCurrentOutCode(MCMD_EARGS);
            LOG_err << "      " << getUsageStr("https");
            return;
        }
        else
        {
            OUTSTREAM << "File transfer is done using " << (api->usingHttpsOnly()?"HTTPS":"HTTP") << std::endl;
        }
        return;
    }
#ifndef _WIN32
    else if (words[0] == "permissions")
    {
        bool filesflagread = getFlag(clflags, "files");
        bool foldersflagread = getFlag(clflags, "folders");

        bool filesflag = filesflagread || (!filesflagread && !foldersflagread);
        bool foldersflag = foldersflagread || (!filesflagread && !foldersflagread);

        bool setperms = getFlag(clflags, "s");

        if ( (!setperms && words.size() > 1) || (setperms && words.size() != 2) || ( setperms && filesflagread  && foldersflagread ) || (setperms && !filesflagread && !foldersflagread))
        {
            setCurrentOutCode(MCMD_EARGS);
            LOG_err << "      " << getUsageStr("permissions");
            return;
        }

        int permvalue = -1;
        if (setperms)
        {
             if (words[1].size() != 3)
             {
                 setCurrentOutCode(MCMD_EARGS);
                 LOG_err << "Invalid permissions value: " << words[1];
             }
             else
             {
                 int owner = words[1].at(0) - '0';
                 int group = words[1].at(1) - '0';
                 int others = words[1].at(2) - '0';
                 if ( (owner < 6) || (owner == 6 && foldersflag) || (owner > 7) || (group < 0) || (group > 7) || (others < 0) || (others > 7) )
                 {
                     setCurrentOutCode(MCMD_EARGS);
                     LOG_err << "Invalid permissions value: " << words[1];
                 }
                 else
                 {
                     permvalue = (owner << 6) + ( group << 3) + others;
                 }
             }
        }

        if (filesflag)
        {
            if (setperms && permvalue != -1)
            {
                api->setDefaultFilePermissions(permvalue);
                ConfigurationManager::savePropertyValue("permissionsFiles", readablePermissions(permvalue));
            }
            int filepermissions = api->getDefaultFilePermissions();
            int owner  = (filepermissions >> 6) & 0x07;
            int group  = (filepermissions >> 3) & 0x07;
            int others = filepermissions & 0x07;

            OUTSTREAM << "Default files permissions: " << owner << group << others << std::endl;
        }
        if (foldersflag)
        {
            if (setperms && permvalue != -1)
            {
                api->setDefaultFolderPermissions(permvalue);
                ConfigurationManager::savePropertyValue("permissionsFolders", readablePermissions(permvalue));
            }
            int folderpermissions = api->getDefaultFolderPermissions();
            int owner  = (folderpermissions >> 6) & 0x07;
            int group  = (folderpermissions >> 3) & 0x07;
            int others = folderpermissions & 0x07;
            OUTSTREAM << "Default folders permissions: " << owner << group << others << std::endl;
        }
    }
#endif
    else if (words[0] == "deleteversions")
    {
        bool deleteall = getFlag(clflags, "all");
        bool forcedelete = getFlag(clflags, "f");
        if (deleteall && words.size()>1)
        {
            setCurrentOutCode(MCMD_EARGS);
            LOG_err << "      " << getUsageStr("deleteversions");
            return;
        }
        if (deleteall)
        {
            string confirmationQuery("Are you sure todelete the version histories of all files? (Yes/No): ");

            int confirmationResponse = forcedelete?MCMDCONFIRM_YES:askforConfirmation(confirmationQuery);

            while ( (confirmationResponse != MCMDCONFIRM_YES) && (confirmationResponse != MCMDCONFIRM_NO) )
            {
                confirmationResponse = askforConfirmation(confirmationQuery);
            }
            if (confirmationResponse == MCMDCONFIRM_YES)
            {

                MegaCmdListener *megaCmdListener = new MegaCmdListener(NULL);
                api->removeVersions(megaCmdListener);
                megaCmdListener->wait();
                if (checkNoErrors(megaCmdListener->getError(), "remove all versions"))
                {
                    OUTSTREAM << "File versions deleted succesfully. Please note that the current files were not deleted, just their history." << std::endl;
                }
                delete megaCmdListener;
            }
        }
        else
        {
            for (unsigned int i = 1; i < words.size(); i++)
            {
                if (isRegExp(words[i]))
                {
                    vector<MegaNode *> *nodesToDeleteVersions = nodesbypath(words[i].c_str(), getFlag(clflags,"use-pcre"));
                    if (nodesToDeleteVersions && nodesToDeleteVersions->size())
                    {
                        for (std::vector< MegaNode * >::iterator it = nodesToDeleteVersions->begin(); it != nodesToDeleteVersions->end(); ++it)
                        {
                            MegaNode * nodeToDeleteVersions = *it;
                            if (nodeToDeleteVersions)
                            {
                                int ret = deleteNodeVersions(nodeToDeleteVersions, api, forcedelete);
                                forcedelete = forcedelete || (ret == MCMDCONFIRM_ALL);
                            }
                        }
                    }
                    else
                    {
                        setCurrentOutCode(MCMD_NOTFOUND);
                        LOG_err << "No node found: " << words[i];
                    }
                    delete nodesToDeleteVersions;
                }
                else // non-regexp
                {
                    MegaNode *n = nodebypath(words[i].c_str());
                    if (n)
                    {
                        int ret = deleteNodeVersions(n, api, forcedelete);
                        forcedelete = forcedelete || (ret == MCMDCONFIRM_ALL);
                    }
                    else
                    {
                        setCurrentOutCode(MCMD_NOTFOUND);
                        LOG_err << "Node not found: " << words[i];
                    }
                    delete n;
                }
            }
        }
    }
#ifdef HAVE_LIBUV
    else if (words[0] == "webdav")
    {
        bool remove = getFlag(clflags, "d");

        if (words.size() > 2 || (words.size() == 1 && remove) )
        {
            setCurrentOutCode(MCMD_EARGS);
            LOG_err << "      " << getUsageStr("webdav");
            return;
        }

        if (words.size() == 1)
        {
            //List served nodes
            MegaNodeList *webdavnodes = api->httpServerGetWebDavAllowedNodes();
            if (webdavnodes)
            {
                bool found = false;

                for (int a = 0; a < webdavnodes->size(); a++)
                {
                    MegaNode *n= webdavnodes->get(a);
                    if (n)
                    {
                        char *link = api->httpServerGetLocalWebDavLink(n); //notice this is not only consulting but also creating,
                        //had it been deleted in the meantime this will recreate it
                        if (link)
                        {
                            if (!found)
                            {
                                OUTSTREAM << "WEBDAV SERVED LOCATIONS:" << std::endl;
                            }
                            found = true;
                            char * nodepath = api->getNodePath(n);
                            OUTSTREAM << nodepath << ": " << link << std::endl;
                            delete []nodepath;
                            delete []link;
                        }
                    }
                }

                if(!found)
                {
                    OUTSTREAM << "No webdav links found" << std::endl;
                }

                delete webdavnodes;

           }
           else
           {
               OUTSTREAM << "Webdav server might not running. Add a new location to serve." << std::endl;
           }

           return;
        }

        if (!remove)
        {
            //create new link:
            bool tls = getFlag(clflags, "tls");
            int port = getintOption(cloptions, "port", 4443);
            bool localonly = !getFlag(clflags, "public");

            string pathtocert = getOption(cloptions, "certificate", "");
            string pathtokey = getOption(cloptions, "key", "");

            bool serverstarted = api->httpServerIsRunning();
            if (!serverstarted)
            {
                LOG_info << "Starting http server";
                api->httpServerEnableFolderServer(true);
    //            api->httpServerEnableOfflineAttribute(true); //TODO: we might want to offer this as parameter
                if (api->httpServerStart(localonly, port, tls, pathtocert.c_str(), pathtokey.c_str()))
                {
                    ConfigurationManager::savePropertyValue("webdav_port", port);
                    ConfigurationManager::savePropertyValue("webdav_localonly", localonly);
                    ConfigurationManager::savePropertyValue("webdav_tls", tls);
                    if (pathtocert.size())
                    {
                        ConfigurationManager::savePropertyValue("webdav_cert", pathtocert);
                    }
                    if (pathtokey.size())
                    {
                        ConfigurationManager::savePropertyValue("webdav_key", pathtokey);
                    }
                }
                else
                {
                    setCurrentOutCode(MCMD_EARGS);
                    LOG_err << "Failed to initialize WEBDAV server";
                    return;
                }
            }
        }

        //add/remove
        for (unsigned int i = 1; i < words.size(); i++)
        {
            string pathToServe = words[i];

            if (remove)
            {
                MegaNode *n = nodebypath(pathToServe.c_str());
                if (n)
                {
                    api->httpServerRemoveWebDavAllowedNode(n->getHandle());

                    mtxWebDavLocations.lock();
                    list<string> servedpaths = ConfigurationManager::getConfigurationValueList<string>("webdav_served_locations");
                    size_t sizeprior = servedpaths.size();
                    servedpaths.remove(pathToServe);
                    size_t sizeafter = servedpaths.size();
                    if (!sizeafter)
                    {
                        api->httpServerStop();
                        ConfigurationManager::savePropertyValue("webdav_port", -1); //so as not to load server on startup
                    }
                    ConfigurationManager::savePropertyValueList("webdav_served_locations", servedpaths);
                    mtxWebDavLocations.unlock();

                    if (sizeprior != sizeafter)
                    {
                        OUTSTREAM << pathToServe << " no longer served via webdav" << std::endl;
                    }
                    else
                    {
                        setCurrentOutCode(MCMD_NOTFOUND);
                        LOG_err << pathToServe << " is not served via webdav";
                    }
                    delete n;
                }
                else
                {
                    setCurrentOutCode(MCMD_NOTFOUND);
                    LOG_err << "Path not found: " << pathToServe;
                    return;
                }
            }
            else //add
            {

                MegaNode *n = nodebypath(pathToServe.c_str());
                if (n)
                {
                    char *l = api->httpServerGetLocalWebDavLink(n);
                    OUTSTREAM << "Serving via webdav " << pathToServe << ": " << l << std::endl;

                    mtxWebDavLocations.lock();
                    list<string> servedpaths = ConfigurationManager::getConfigurationValueList<string>("webdav_served_locations");
                    servedpaths.push_back(pathToServe);
                    servedpaths.sort();
                    servedpaths.unique();
                    ConfigurationManager::savePropertyValueList("webdav_served_locations", servedpaths);
                    mtxWebDavLocations.unlock();


                    delete n;
                    delete []l;
                }
                else
                {
                    setCurrentOutCode(MCMD_NOTFOUND);
                    LOG_err << "Path not found: " << pathToServe;
                    return;
                }
            }
        }
    }
#endif
#ifdef ENABLE_SYNC
    else if (words[0] == "exclude")
    {
        api->enableTransferResumption();

        if (getFlag(clflags, "a"))
        {
            for (unsigned int i=1;i<words.size(); i++)
            {
                ConfigurationManager::addExcludedName(words[i]);
            }
            if (words.size()>1)
            {
                std::vector<string> vexcludednames(ConfigurationManager::excludedNames.begin(), ConfigurationManager::excludedNames.end());
                api->setExcludedNames(&vexcludednames);
                if (getFlag(clflags, "restart-syncs"))
                {
                    restartsyncs();
                }
            }
            else
            {
                setCurrentOutCode(MCMD_EARGS);
                LOG_err << "      " << getUsageStr("exclude");
                return;
            }
        }
        else if (getFlag(clflags, "d"))
        {
            for (unsigned int i=1;i<words.size(); i++)
            {
                ConfigurationManager::removeExcludedName(words[i]);
            }
            if (words.size()>1)
            {
                std::vector<string> vexcludednames(ConfigurationManager::excludedNames.begin(), ConfigurationManager::excludedNames.end());
                api->setExcludedNames(&vexcludednames);
                if (getFlag(clflags, "restart-syncs"))
                {
                    restartsyncs();
                }
            }
            else
            {
                setCurrentOutCode(MCMD_EARGS);
                LOG_err << "      " << getUsageStr("exclude");
                return;
            }
        }


        OUTSTREAM << "List of excluded names:" << std::endl;

        for (set<string>::iterator it=ConfigurationManager::excludedNames.begin(); it!=ConfigurationManager::excludedNames.end(); ++it)
        {
            OUTSTREAM << *it << std::endl;
        }

        if ( !getFlag(clflags, "restart-syncs") && (getFlag(clflags, "a") || getFlag(clflags, "d")) )
        {
            OUTSTREAM << std::endl <<  "Changes will not be applied inmediately to operations being performed in active syncs."
                      << " See \"exclude --help\" for further info" << std::endl;
        }
    }
    else if (words[0] == "sync")
    {
        if (!api->isFilesystemAvailable())
        {
            setCurrentOutCode(MCMD_NOTLOGGEDIN);
            LOG_err << "Not logged in.";
            return;
        }
        if (!api->isLoggedIn())
        {
            LOG_err << "Not logged in";
            setCurrentOutCode(MCMD_NOTLOGGEDIN);
            return;
        }

        int PATHSIZE = getintOption(cloptions,"path-display-size");
        if (!PATHSIZE)
        {
            // get screen size for output purposes
            unsigned int width = getNumberOfCols(75);
            PATHSIZE = std::min(60,int((width-46)/2));
        }

        bool headershown = false;
        bool modifiedsyncs = false;
        mtxSyncMap.lock();
        if (words.size() == 3)
        {
            string path;
            string localrelativepath;
            string localabsolutepath;
            fsAccessCMD->path2local(&words[1], &localrelativepath);
            fsAccessCMD->expanselocalpath(&localrelativepath, &localabsolutepath);
            fsAccessCMD->local2path(&localabsolutepath, &path);
            MegaNode* n = nodebypath(words[2].c_str());
            if (n)
            {
                if (n->getType() == MegaNode::TYPE_FILE)
                {
                    LOG_err << words[2] << ": Remote sync root must be folder.";
                }
                else if (api->getAccess(n) >= MegaShare::ACCESS_FULL)
                {
                    MegaCmdListener *megaCmdListener = new MegaCmdListener(NULL);

                    api->syncFolder(path.c_str(), n, megaCmdListener);
                    megaCmdListener->wait();
                    if (checkNoErrors(megaCmdListener->getError(), "sync folder"))
                    {
                        sync_struct *thesync = new sync_struct;
                        thesync->active = true;
                        thesync->handle = megaCmdListener->getRequest()->getNodeHandle();
                        thesync->localpath = string(megaCmdListener->getRequest()->getFile());
                        thesync->fingerprint = megaCmdListener->getRequest()->getNumber();

                        if (ConfigurationManager::configuredSyncs.find(megaCmdListener->getRequest()->getFile()) != ConfigurationManager::configuredSyncs.end())
                        {
                            delete ConfigurationManager::configuredSyncs[megaCmdListener->getRequest()->getFile()];
                        }
                        ConfigurationManager::configuredSyncs[megaCmdListener->getRequest()->getFile()] = thesync;

                        char * nodepath = api->getNodePath(n);
                        LOG_info << "Added sync: " << megaCmdListener->getRequest()->getFile() << " to " << nodepath;

                        modifiedsyncs=true;
                        delete []nodepath;
                    }

                    delete megaCmdListener;
                }
                else
                {
                    setCurrentOutCode(MCMD_NOTPERMITTED);
                    LOG_err << words[2] << ": Syncing requires full access to path, current acces: " << api->getAccess(n);
                }
                delete n;
            }
            else
            {
                setCurrentOutCode(MCMD_NOTFOUND);
                LOG_err << "Couldn't find remote folder: " << words[2];
            }
        }
        else if (words.size() == 2)
        {
            int id = toInteger(words[1].c_str());
            map<string, sync_struct *>::iterator itr;
            int i = 0;
            bool foundsync = false;
            for (itr = ConfigurationManager::configuredSyncs.begin(); itr != ConfigurationManager::configuredSyncs.end(); i++)
            {
                string key = ( *itr ).first;
                sync_struct *thesync = ((sync_struct*)( *itr ).second );
                MegaNode * n = api->getNodeByHandle(thesync->handle);
                bool erased = false;

                if (n)
                {
                    char * nodepath = api->getNodePath(n);

                    if (( id == i ) || (( id == -1 ) && ( words[1] == thesync->localpath )))
                    {
                        foundsync = true;
                        long long nfiles = 0;
                        long long nfolders = 0;
                        nfolders++; //add the share itself
                        getNumFolderFiles(n, api, &nfiles, &nfolders);

                        if (getFlag(clflags, "s") || getFlag(clflags, "r"))
                        {
                            bool stopping = getFlag(clflags, "s");
                            LOG_info << (stopping?"Stopping (disabling) sync ":"Resuming sync ") << key << ": " << nodepath;
                            MegaCmdListener *megaCmdListener = new MegaCmdListener(NULL);
                            if (stopping)
                            {
                                api->disableSync(n, megaCmdListener);
                            }
                            else
                            {
                                api->syncFolder(thesync->localpath.c_str(), n, megaCmdListener);
                            }

                            megaCmdListener->wait();

                            if (checkNoErrors(megaCmdListener->getError(), stopping?"stop sync":"resume sync"))
                            {
                                thesync->active = !stopping;
                                thesync->loadedok = true;
                                if (!stopping) //syncFolder
                                {
                                    if (megaCmdListener->getRequest()->getNumber())
                                    {
                                        thesync->fingerprint = megaCmdListener->getRequest()->getNumber();
                                    }
                                }
                                modifiedsyncs=true;
                            }
                            else
                            {
                                thesync->active = false;
                                thesync->loadedok = false;
                            }
                            delete megaCmdListener;
                        }
                        else if (getFlag(clflags, "d"))
                        {
                            LOG_debug << "Removing sync " << key << " to " << nodepath;
                            MegaCmdListener *megaCmdListener = new MegaCmdListener(NULL);
                            if (thesync->active)  //if not active, removeSync will fail.)
                            {
                                api->removeSync(n, megaCmdListener);
                                megaCmdListener->wait();
                                if (checkNoErrors(megaCmdListener->getError(), "remove sync"))
                                {
                                    ConfigurationManager::configuredSyncs.erase(itr++); //TODO: should protect with mutex!
                                    erased = true;
                                    delete ( thesync );
                                    LOG_info << "Removed sync " << key << " to " << nodepath;
                                    modifiedsyncs=true;
                                }
                            }
                            else //if !active simply remove
                            {
                                //TODO: if the sdk ever provides a way to clean cache, call it
                                ConfigurationManager::configuredSyncs.erase(itr++);
                                erased = true;
                                delete ( thesync );
                                LOG_info << "Removed sync " << key << " to " << nodepath;
                                modifiedsyncs=true;
                            }
                            delete megaCmdListener;
                        }

                        if (!headershown)
                        {
                            headershown = true;
                            printSyncHeader(PATHSIZE);
                        }

                        printSync(i, key, nodepath, thesync, n, nfiles, nfolders, PATHSIZE);

                    }
                    delete n;
                    delete []nodepath;
                }
                else
                {
                    setCurrentOutCode(MCMD_NOTFOUND);
                    LOG_err << "Node not found for sync " << key << " into handle: " << thesync->handle;
                }
                if (!erased)
                {
                    ++itr;
                }
            }
            if (!foundsync)
            {
                setCurrentOutCode(MCMD_NOTFOUND);
                LOG_err << "Sync not found: " << words[1] << ". Please provide full path or valid ID";
            }
        }
        else if (words.size() == 1)
        {
            map<string, sync_struct *>::const_iterator itr;
            int i = 0;
            for (itr = ConfigurationManager::configuredSyncs.begin(); itr != ConfigurationManager::configuredSyncs.end(); ++itr)
            {
                sync_struct *thesync = ((sync_struct*)( *itr ).second );
                MegaNode * n = api->getNodeByHandle(thesync->handle);

                if (n)
                {
                    if (!headershown)
                    {
                        headershown = true;
                        printSyncHeader(PATHSIZE);
                    }
                    long long nfiles = 0;
                    long long nfolders = 0;
                    nfolders++; //add the share itself
                    getNumFolderFiles(n, api, &nfiles, &nfolders);

                    char * nodepath = api->getNodePath(n);
                    printSync(i++, ( *itr ).first, nodepath, thesync, n, nfiles, nfolders, PATHSIZE);

                    delete n;
                    delete []nodepath;
                }
                else
                {
                    setCurrentOutCode(MCMD_NOTFOUND);
                    LOG_err << "Node not found for sync " << ( *itr ).first << " into handle: " << thesync->handle;
                }
            }
        }
        else
        {
            setCurrentOutCode(MCMD_EARGS);
            LOG_err << "      " << getUsageStr("sync");
            mtxSyncMap.unlock();
            return;
        }
        if (modifiedsyncs)
        {
            ConfigurationManager::saveSyncs(&ConfigurationManager::configuredSyncs);
        }
        mtxSyncMap.unlock();
        return;
    }
#endif
    else if (words[0] == "login")
    {
        int clientID = getintOption(cloptions, "clientID", -1);

        if (!api->isLoggedIn())
        {
            if (words.size() > 1)
            {
                if (strchr(words[1].c_str(), '@'))
                {
                    // full account login
                    if (words.size() > 2)
                    {
                        MegaCmdListener *megaCmdListener = new MegaCmdListener(NULL,NULL,clientID);
                        api->login(words[1].c_str(), words[2].c_str(), megaCmdListener);
                        actUponLogin(megaCmdListener);
                        delete megaCmdListener;
                    }
                    else
                    {
                        login = words[1];
                        if (interactiveThread())
                        {
                            setprompt(LOGINPASSWORD);
                        }
                        else
                        {
                            setCurrentOutCode(MCMD_EARGS);
                            LOG_err << "Extra args required in non interactive mode. Usage: " << getUsageStr("login");
                        }
                    }
                }
                else
                {
                    const char* ptr;
                    if (( ptr = strchr(words[1].c_str(), '#')))  // folder link indicator
                    {
                        MegaCmdListener *megaCmdListener = new MegaCmdListener(NULL);
                        api->loginToFolder(words[1].c_str(), megaCmdListener);
                        actUponLogin(megaCmdListener);
                        delete megaCmdListener;
                        return;
                    }
                    else
                    {
                        byte session[64];

                        if (words[1].size() < sizeof session * 4 / 3)
                        {
                            LOG_info << "Resuming session...";
                            MegaCmdListener *megaCmdListener = new MegaCmdListener(NULL);
                            api->fastLogin(words[1].c_str(), megaCmdListener);
                            actUponLogin(megaCmdListener);
                            delete megaCmdListener;
                            return;
                        }
                    }
                    setCurrentOutCode(MCMD_EARGS);
                    LOG_err << "Invalid argument. Please specify a valid e-mail address, "
                              << "a folder link containing the folder key "
                              << "or a valid session.";
                }
            }
            else
            {
                setCurrentOutCode(MCMD_EARGS);
                LOG_err << "      " << getUsageStr("login");
            }
        }
        else
        {
            setCurrentOutCode(MCMD_INVALIDSTATE);
            LOG_err << "Already logged in. Please log out first.";
        }

        return;
    }
    else if (words[0] == "mount")
    {
        if (!api->isFilesystemAvailable())
        {
            setCurrentOutCode(MCMD_NOTLOGGEDIN);
            LOG_err << "Not logged in.";
            return;
        }
        listtrees();
        return;
    }
    else if (words[0] == "share")
    {
        if (!api->isFilesystemAvailable())
        {
            setCurrentOutCode(MCMD_NOTLOGGEDIN);
            LOG_err << "Not logged in.";
            return;
        }
        string with = getOption(cloptions, "with", "");
        if (getFlag(clflags, "a") && ( "" == with ))
        {
            setCurrentOutCode(MCMD_EARGS);
            LOG_err << " Required --with=user";
            LOG_err <<  "      " << getUsageStr("share");
            return;
        }

        string slevel = getOption(cloptions, "level", "NONE");

        int level_NOT_present_value = -214;
        int level;
        if (slevel == "NONE")
        {
            level = level_NOT_present_value;
        }
        else
        {
            level = getShareLevelNum(slevel.c_str());
        }
        if (( level != level_NOT_present_value ) && (( level < -1 ) || ( level > 3 )))
        {
            setCurrentOutCode(MCMD_EARGS);
            LOG_err << "Invalid level of access";
            return;
        }
        bool listPending = getFlag(clflags, "p");

        if (words.size() <= 1)
        {
            words.push_back(string(".")); //cwd
        }
        for (int i = 1; i < (int)words.size(); i++)
        {
            unescapeifRequired(words[i]);
            if (isRegExp(words[i]))
            {
                vector<MegaNode *> *nodes = nodesbypath(words[i].c_str(), getFlag(clflags,"use-pcre"));
                if (nodes)
                {
                    if (!nodes->size())
                    {
                        setCurrentOutCode(MCMD_NOTFOUND);
                        if (words[i].find("@") != string::npos)
                        {
                            LOG_err << "Could not find " << words[i] << ". Use --with=" << words[i] << " to specify the user to share with";
                        }
                        else
                        {
                            LOG_err << "Node not found: " << words[i];
                        }
                    }
                    for (std::vector< MegaNode * >::iterator it = nodes->begin(); it != nodes->end(); ++it)
                    {
                        MegaNode * n = *it;
                        if (n)
                        {
                            if (getFlag(clflags, "a"))
                            {
                                LOG_debug << " sharing ... " << n->getName() << " with " << with;
                                if (level == level_NOT_present_value)
                                {
                                    level = MegaShare::ACCESS_READ;
                                }

                                if (n->getType() == MegaNode::TYPE_FILE)
                                {
                                    setCurrentOutCode(MCMD_INVALIDTYPE);
                                    LOG_err << "Cannot share file: " << n->getName() << ". Only folders allowed. You can send file to user's inbox with cp (see \"cp --help\")";
                                }
                                else
                                {
                                    shareNode(n, with, level);
                                }
                            }
                            else if (getFlag(clflags, "d"))
                            {
                                if ("" != with)
                                {
                                    LOG_debug << " deleting share ... " << n->getName() << " with " << with;
                                    disableShare(n, with);
                                }
                                else
                                {
                                    MegaShareList* outShares = api->getOutShares(n);
                                    if (outShares)
                                    {
                                        for (int i = 0; i < outShares->size(); i++)
                                        {
                                            if (outShares->get(i)->getNodeHandle() == n->getHandle())
                                            {
                                                LOG_debug << " deleting share ... " << n->getName() << " with " << outShares->get(i)->getUser();
                                                disableShare(n, outShares->get(i)->getUser());
                                            }
                                        }

                                        delete outShares;
                                    }
                                }
                            }
                            else
                            {
                                if (( level != level_NOT_present_value ) || ( with != "" ))
                                {
                                    setCurrentOutCode(MCMD_EARGS);
                                    LOG_err << "Unexpected option received. To create/modify a share use -a";
                                }
                                else if (listPending)
                                {
                                    dumpListOfPendingShares(n, words[i]);
                                }
                                else
                                {
                                    dumpListOfShared(n, words[i]);
                                }
                            }
                            delete n;
                        }
                    }

                    nodes->clear();
                    delete nodes;
                }
                else
                {
                    setCurrentOutCode(MCMD_NOTFOUND);
                    LOG_err << "Node not found: " << words[i];
                }
            }
            else // non-regexp
            {
                MegaNode *n = nodebypath(words[i].c_str());
                if (n)
                {
                    if (getFlag(clflags, "a"))
                    {
                        LOG_debug << " sharing ... " << n->getName() << " with " << with;
                        if (level == level_NOT_present_value)
                        {
                            level = MegaShare::ACCESS_READ;
                        }
                        shareNode(n, with, level);
                    }
                    else if (getFlag(clflags, "d"))
                    {
                        if ("" != with)
                        {
                            LOG_debug << " deleting share ... " << n->getName() << " with " << with;
                            disableShare(n, with);
                        }
                        else
                        {
                            MegaShareList* outShares = api->getOutShares(n);
                            if (outShares)
                            {
                                for (int i = 0; i < outShares->size(); i++)
                                {
                                    if (outShares->get(i)->getNodeHandle() == n->getHandle())
                                    {
                                        LOG_debug << " deleting share ... " << n->getName() << " with " << outShares->get(i)->getUser();
                                        disableShare(n, outShares->get(i)->getUser());
                                    }
                                }

                                delete outShares;
                            }
                        }
                    }
                    else
                    {
                        if (( level != level_NOT_present_value ) || ( with != "" ))
                        {
                            setCurrentOutCode(MCMD_EARGS);
                            LOG_err << "Unexpected option received. To create/modify a share use -a";
                        }
                        else if (listPending)
                        {
                            dumpListOfPendingShares(n, words[i]);
                        }
                        else
                        {
                            dumpListOfShared(n, words[i]);
                        }
                    }
                    delete n;
                }
                else
                {
                    setCurrentOutCode(MCMD_NOTFOUND);
                    LOG_err << "Node not found: " << words[i];
                }
            }
        }

        return;
    }
    else if (words[0] == "users")
    {
        if (!api->isFilesystemAvailable())
        {
            setCurrentOutCode(MCMD_NOTLOGGEDIN);
            LOG_err << "Not logged in.";
            return;
        }
        if (getFlag(clflags, "d") && ( words.size() <= 1 ))
        {
            setCurrentOutCode(MCMD_EARGS);
            LOG_err << "Contact to delete not specified";
            return;
        }
        MegaUserList* usersList = api->getContacts();
        if (usersList)
        {
            for (int i = 0; i < usersList->size(); i++)
            {
                MegaUser *user = usersList->get(i);

                if (getFlag(clflags, "d") && ( words.size() > 1 ) && ( words[1] == user->getEmail()))
                {
                    MegaCmdListener *megaCmdListener = new MegaCmdListener(NULL);
                    api->removeContact(user, megaCmdListener);
                    megaCmdListener->wait();
                    if (checkNoErrors(megaCmdListener->getError(), "delete contact"))
                    {
                        OUTSTREAM << "Contact " << words[1] << " removed succesfully" << std::endl;
                    }
                    delete megaCmdListener;
                }
                else
                {
                    if (!(( user->getVisibility() != MegaUser::VISIBILITY_VISIBLE ) && !getFlag(clflags, "h")))
                    {
                        if (getFlag(clflags,"n"))
                        {
                            string name;
                            MegaCmdListener *megaCmdListener = new MegaCmdListener(NULL);
                            api->getUserAttribute(user, ATTR_FIRSTNAME, megaCmdListener);
                            megaCmdListener->wait();
                            if (megaCmdListener->getError()->getErrorCode() == MegaError::API_OK)
                            {
                                if (megaCmdListener->getRequest()->getText() && strlen(megaCmdListener->getRequest()->getText()))
                                {
                                    name += megaCmdListener->getRequest()->getText();
                                }
                            }
                            delete megaCmdListener;

                            megaCmdListener = new MegaCmdListener(NULL);
                            api->getUserAttribute(user, ATTR_LASTNAME, megaCmdListener);
                            megaCmdListener->wait();
                            if (megaCmdListener->getError()->getErrorCode() == MegaError::API_OK)
                            {
                                if (megaCmdListener->getRequest()->getText() && strlen(megaCmdListener->getRequest()->getText()))
                                {
                                    if (name.size())
                                    {
                                        name+=" ";
                                    }
                                    name+=megaCmdListener->getRequest()->getText();
                                }
                            }
                            if (name.size())
                            {
                                OUTSTREAM << name << ": ";
                            }

                            delete megaCmdListener;
                        }


                        OUTSTREAM << user->getEmail() << ", " << visibilityToString(user->getVisibility());
                        if (user->getTimestamp())
                        {
                            OUTSTREAM << " since " << getReadableTime(user->getTimestamp());
                        }
                        OUTSTREAM << std::endl;

                        if (getFlag(clflags, "s"))
                        {
                            MegaShareList *shares = api->getOutShares();
                            if (shares)
                            {
                                bool first_share = true;
                                for (int j = 0; j < shares->size(); j++)
                                {
                                    if (!strcmp(shares->get(j)->getUser(), user->getEmail()))
                                    {
                                        MegaNode * n = api->getNodeByHandle(shares->get(j)->getNodeHandle());
                                        if (n)
                                        {
                                            if (first_share)
                                            {
                                                OUTSTREAM << "\tSharing:" << std::endl;
                                                first_share = false;
                                            }

                                            OUTSTREAM << "\t";
                                            dumpNode(n, 2, false, 0, getDisplayPath("/", n).c_str());
                                            delete n;
                                        }
                                    }
                                }

                                delete shares;
                            }
                        }
                    }
                }
            }

            delete usersList;
        }

        return;
    }
    else if (words[0] == "mkdir")
    {
        if (!api->isFilesystemAvailable())
        {
            setCurrentOutCode(MCMD_NOTLOGGEDIN);
            LOG_err << "Not logged in.";
            return;
        }
        int globalstatus = MCMD_OK;
        if (words.size()<2)
        {
            globalstatus = MCMD_EARGS;
        }
        bool printusage = false;
        for (unsigned int i = 1; i < words.size(); i++)
        {
            unescapeifRequired(words[i]);
            //git first existing node in the asked path:
            MegaNode *baseNode;

            string rest = words[i];
            if (rest.find("//bin/") == 0)
            {
                baseNode = api->getRubbishNode();
                rest = rest.substr(6);
            }//elseif //in/
            else if(rest.find("/") == 0)
            {
                baseNode = api->getRootNode();
                rest = rest.substr(1);
            }
            else
            {
                baseNode = api->getNodeByHandle(cwd);
            }

            while (baseNode && rest.length())
            {
                size_t possep = rest.find_first_of("/");
                if (possep == string::npos)
                {
                    break;
                }

                string next = rest.substr(0, possep);
                if (next == ".")
                {
                    rest = rest.substr(possep + 1);
                    continue;
                }
                else if(next == "..")
                {
                    MegaNode *aux = baseNode;
                    baseNode = api->getNodeByHandle(baseNode->getParentHandle());

                    if (aux!=baseNode) // let's be paranoid
                    {
                        delete aux;
                    }
                }
                else
                {
                    MegaNodeList *children = api->getChildren(baseNode);
                    if (children)
                    {
                        bool found = false;
                        for (int i = 0; i < children->size(); i++)
                        {
                            MegaNode *child = children->get(i);
                            if (next == child->getName())
                            {
                                MegaNode *aux = baseNode;
                                baseNode = child->copy();
                                found = true;
                                if (aux!=baseNode) // let's be paranoid
                                {
                                    delete aux;
                                }
                                break;
                            }
                        }
                        delete children;
                        if (!found)
                        {
                            break;
                        }
                    }
                }

                rest = rest.substr(possep + 1);
            }
            if (baseNode)
            {
                int status = makedir(rest,getFlag(clflags, "p"),baseNode);
                if (status != MCMD_OK)
                {
                    globalstatus = status;
                }
                if (status == MCMD_EARGS)
                {
                    printusage = true;
                }
                delete baseNode;
            }
            else
            {
                setCurrentOutCode(MCMD_INVALIDSTATE);
                LOG_err << "Folder navigation failed";
                return;
            }

        }

        setCurrentOutCode(globalstatus);
        if (printusage)
        {
            LOG_err << "      " << getUsageStr("mkdir");
        }

        return;
    }
    else if (words[0] == "attr")
    {
        if (!api->isFilesystemAvailable())
        {
            setCurrentOutCode(MCMD_NOTLOGGEDIN);
            LOG_err << "Not logged in.";
            return;
        }
        if (words.size() > 1)
        {
            int cancel = getFlag(clflags, "d");
            bool settingattr = getFlag(clflags, "s");

            string nodePath = words.size() > 1 ? words[1] : "";
            string attribute = words.size() > 2 ? words[2] : "";
            string attrValue = words.size() > 3 ? words[3] : "";
            n = nodebypath(nodePath.c_str());

            if (n)
            {
                if (settingattr || cancel)
                {
                    if (attribute.size())
                    {
                        const char *cattrValue = cancel ? NULL : attrValue.c_str();
                        MegaCmdListener *megaCmdListener = new MegaCmdListener(NULL);
                        api->setCustomNodeAttribute(n, attribute.c_str(), cattrValue, megaCmdListener);
                        megaCmdListener->wait();
                        if (checkNoErrors(megaCmdListener->getError(), "set node attribute: " + attribute))
                        {
                            OUTSTREAM << "Node attribute " << attribute << ( cancel ? " removed" : " updated" ) << " correctly" << std::endl;
                            delete n;
                            n = api->getNodeByHandle(megaCmdListener->getRequest()->getNodeHandle());
                        }
                        delete megaCmdListener;
                    }
                    else
                    {
                        setCurrentOutCode(MCMD_EARGS);
                        LOG_err << "Attribute not specified";
                        LOG_err << "      " << getUsageStr("attr");
                        return;
                    }
                }

                //List node custom attributes
                MegaStringList *attrlist = n->getCustomAttrNames();
                if (attrlist)
                {
                    if (!attribute.size())
                    {
                        OUTSTREAM << "The node has " << attrlist->size() << " attributes" << std::endl;
                    }
                    for (int a = 0; a < attrlist->size(); a++)
                    {
                        string iattr = attrlist->get(a);
                        if (!attribute.size() || ( attribute == iattr ))
                        {
                            const char* iattrval = n->getCustomAttr(iattr.c_str());
                            OUTSTREAM << "\t" << iattr << " = " << ( iattrval ? iattrval : "NULL" ) << std::endl;
                        }
                    }

                    delete attrlist;
                }

                delete n;
            }
            else
            {
                setCurrentOutCode(MCMD_NOTFOUND);
                LOG_err << "Couldn't find node: " << nodePath;
                return;
            }
        }
        else
        {
            setCurrentOutCode(MCMD_EARGS);
            LOG_err << "      " << getUsageStr("attr");
            return;
        }


        return;
    }
    else if (words[0] == "userattr")
    {
        if (!api->isFilesystemAvailable())
        {
            setCurrentOutCode(MCMD_NOTLOGGEDIN);
            LOG_err << "Not logged in.";
            return;
        }
        bool settingattr = getFlag(clflags, "s");

        int attribute = getAttrNum(words.size() > 1 ? words[1].c_str() : "-1");
        string attrValue = words.size() > 2 ? words[2] : "";
        string user = getOption(cloptions, "user", "");
        if (settingattr && user.size())
        {
            LOG_err << "Can't change other user attributes";
            return;
        }


        if (settingattr)
        {
            if (attribute != -1)
            {
                MegaCmdListener *megaCmdListener = new MegaCmdListener(NULL);
                api->setUserAttribute(attribute, attrValue.c_str(), megaCmdListener);
                megaCmdListener->wait();
                if (checkNoErrors(megaCmdListener->getError(), string("set user attribute ") + getAttrStr(attribute)))
                {
                    OUTSTREAM << "User attribute " << getAttrStr(attribute) << " updated" << " correctly" << std::endl;
                }
                else
                {
                    delete megaCmdListener;
                    return;
                }
                delete megaCmdListener;
            }
            else
            {
                setCurrentOutCode(MCMD_EARGS);
                LOG_err << "Attribute not specified";
                LOG_err << "      " << getUsageStr("userattr");
                return;
            }
        }

        for (int a = ( attribute == -1 ? 0 : attribute ); a < ( attribute == -1 ? 10 : attribute + 1 ); a++)
        {
            MegaCmdListener *megaCmdListener = new MegaCmdListener(NULL);
            if (user.size())
            {
                api->getUserAttribute(user.c_str(), a, megaCmdListener);
            }
            else
            {
                api->getUserAttribute(a, megaCmdListener);
            }
            megaCmdListener->wait();
            if (checkNoErrors(megaCmdListener->getError(), string("get user attribute ") + getAttrStr(a)))
            {
                int iattr = megaCmdListener->getRequest()->getParamType();
                const char *value = megaCmdListener->getRequest()->getText();
                string svalue;
                try
                {
                    if (value)
                    {
                        svalue = string(value);
                    }
                    else
                    {
                        svalue = "NOT PRINTABLE";
                    }

                }
                catch (std::exception e)
                {
                    svalue = "NOT PRINTABLE";
                }
                OUTSTREAM << "\t" << getAttrStr(iattr) << " = " << svalue << std::endl;
            }

            delete megaCmdListener;
        }

        return;
    }
    else if (words[0] == "thumbnail")
    {
        if (!api->isFilesystemAvailable())
        {
            setCurrentOutCode(MCMD_NOTLOGGEDIN);
            LOG_err << "Not logged in.";
            return;
        }
        if (words.size() > 1)
        {
            string nodepath = words[1];
            string path = words.size() > 2 ? words[2] : "./";
            n = nodebypath(nodepath.c_str());
            if (n)
            {
                MegaCmdListener *megaCmdListener = new MegaCmdListener(NULL);
                bool setting = getFlag(clflags, "s");
                if (setting)
                {
                    api->setThumbnail(n, path.c_str(), megaCmdListener);
                }
                else
                {
                    api->getThumbnail(n, path.c_str(), megaCmdListener);
                }
                megaCmdListener->wait();
                if (checkNoErrors(megaCmdListener->getError(), ( setting ? "set thumbnail " : "get thumbnail " ) + nodepath + " to " + path))
                {
                    OUTSTREAM << "Thumbnail for " << nodepath << ( setting ? " loaded from " : " saved in " ) << megaCmdListener->getRequest()->getFile() << std::endl;
                }
                delete megaCmdListener;
                delete n;
            }
        }
        else
        {
            setCurrentOutCode(MCMD_EARGS);
            LOG_err << "      " << getUsageStr("attr");
            return;
        }
        return;
    }
    else if (words[0] == "preview")
    {
        if (!api->isFilesystemAvailable())
        {
            setCurrentOutCode(MCMD_NOTLOGGEDIN);
            LOG_err << "Not logged in.";
            return;
        }
        if (words.size() > 1)
        {
            string nodepath = words[1];
            string path = words.size() > 2 ? words[2] : "./";
            n = nodebypath(nodepath.c_str());
            if (n)
            {
                MegaCmdListener *megaCmdListener = new MegaCmdListener(NULL);
                bool setting = getFlag(clflags, "s");
                if (setting)
                {
                    api->setPreview(n, path.c_str(), megaCmdListener);
                }
                else
                {
                    api->getPreview(n, path.c_str(), megaCmdListener);
                }
                megaCmdListener->wait();
                if (checkNoErrors(megaCmdListener->getError(), ( setting ? "set preview " : "get preview " ) + nodepath + " to " + path))
                {
                    OUTSTREAM << "Preview for " << nodepath << ( setting ? " loaded from " : " saved in " ) << megaCmdListener->getRequest()->getFile() << std::endl;
                }
                delete megaCmdListener;
                delete n;
            }
        }
        else
        {
            setCurrentOutCode(MCMD_EARGS);
            LOG_err << "      " << getUsageStr("attr");
            return;
        }
        return;
    }
    else if (words[0] == "debug")
    {
        vector<string> newcom;
        newcom.push_back("log");
        newcom.push_back("5");

        return executecommand(newcom, clflags, cloptions);
    }
    else if (words[0] == "passwd")
    {
        if (api->isLoggedIn())
        {
            if (words.size() == 1)
            {
                if (interactiveThread())
                {
                    setprompt(OLDPASSWORD);
                }
                else
                {
                    setCurrentOutCode(MCMD_EARGS);
                    LOG_err << "Extra args required in non interactive mode. Usage: " << getUsageStr("passwd");
                }
            }
            else if (words.size() > 2)
            {
                changePassword(words[1].c_str(), words[2].c_str());
            }
            else
            {
                setCurrentOutCode(MCMD_EARGS);
                LOG_err << "      " << getUsageStr("passwd");
            }
        }
        else
        {
            setCurrentOutCode(MCMD_NOTLOGGEDIN);
            LOG_err << "Not logged in.";
        }

        return;
    }
    else if (words[0] == "speedlimit")
    {
        if (words.size() > 2)
        {
            setCurrentOutCode(MCMD_EARGS);
            LOG_err << "      " << getUsageStr("speedlimit");
            return;
        }
        if (words.size() > 1)
        {
            long long maxspeed = textToSize(words[1].c_str());
            if (maxspeed == -1)
            {
                string s = words[1] + "B";
                maxspeed = textToSize(s.c_str());
            }
            if (!getFlag(clflags, "u") && !getFlag(clflags, "d"))
            {
                api->setMaxDownloadSpeed(maxspeed);
                api->setMaxUploadSpeed(maxspeed);
                ConfigurationManager::savePropertyValue("maxspeedupload", maxspeed);
                ConfigurationManager::savePropertyValue("maxspeeddownload", maxspeed);
            }
            else if (getFlag(clflags, "u"))
            {
                api->setMaxUploadSpeed(maxspeed);
                ConfigurationManager::savePropertyValue("maxspeedupload", maxspeed);
            }
            else if (getFlag(clflags, "d"))
            {
                api->setMaxDownloadSpeed(maxspeed);
                ConfigurationManager::savePropertyValue("maxspeeddownload", maxspeed);
            }
        }

        bool hr = getFlag(clflags,"h");

        if (!getFlag(clflags, "u") && !getFlag(clflags, "d"))
        {
            long long us = api->getMaxUploadSpeed();
            long long ds = api->getMaxDownloadSpeed();
            OUTSTREAM << "Upload speed limit = " << (us?sizeToText(us,false,hr):"unlimited") << ((us && hr)?"/s":(us?" B/s":""))  << std::endl;
            OUTSTREAM << "Download speed limit = " << (ds?sizeToText(ds,false,hr):"unlimited") << ((ds && hr)?"/s":(us?" B/s":"")) << std::endl;
        }
        else if (getFlag(clflags, "u"))
        {
            long long us = api->getMaxUploadSpeed();
            OUTSTREAM << "Upload speed limit = " << (us?sizeToText(us,false,hr):"unlimited") << ((us && hr)?"/s":(us?" B/s":"")) << std::endl;
        }
        else if (getFlag(clflags, "d"))
        {
            long long ds = api->getMaxDownloadSpeed();
            OUTSTREAM << "Download speed limit = " << (ds?sizeToText(ds,false,hr):"unlimited") << ((ds && hr)?"/s":(ds?" B/s":"")) << std::endl;
        }

        return;
    }
    else if (words[0] == "invite")
    {
        if (!api->isFilesystemAvailable())
        {
            setCurrentOutCode(MCMD_NOTLOGGEDIN);
            LOG_err << "Not logged in.";
            return;
        }
        if (words.size() > 1)
        {
            string email = words[1];
            if (!isValidEmail(email))
            {
                setCurrentOutCode(MCMD_INVALIDEMAIL);
                LOG_err << "No valid email provided";
                LOG_err << "      " << getUsageStr("invite");
            }
            else
            {
                int action = MegaContactRequest::INVITE_ACTION_ADD;
                if (getFlag(clflags, "d"))
                {
                    action = MegaContactRequest::INVITE_ACTION_DELETE;
                }
                if (getFlag(clflags, "r"))
                {
                    action = MegaContactRequest::INVITE_ACTION_REMIND;
                }

                string message = getOption(cloptions, "message", "");
                MegaCmdListener *megaCmdListener = new MegaCmdListener(NULL);
                api->inviteContact(email.c_str(), message.c_str(), action, megaCmdListener);
                megaCmdListener->wait();
                if (checkNoErrors(megaCmdListener->getError(), action==MegaContactRequest::INVITE_ACTION_DELETE?"remove invitation":"(re)invite user"))
                {
                    OUTSTREAM << "Invitation to user: " << email << " " << (action==MegaContactRequest::INVITE_ACTION_DELETE?"removed":"sent") << std::endl;
                }
                else if (megaCmdListener->getError()->getErrorCode() == MegaError::API_EACCESS)
                {
                    ostringstream os;
                    os << "Reminder not yet available: " << " available after 15 days";
                    MegaContactRequestList *ocrl = api->getOutgoingContactRequests();
                    if (ocrl)
                    {
                        for (int i = 0; i < ocrl->size(); i++)
                        {
                            if (ocrl->get(i)->getTargetEmail() && megaCmdListener->getRequest()->getEmail() && !strcmp(ocrl->get(i)->getTargetEmail(), megaCmdListener->getRequest()->getEmail()))
                            {
                                os << " (" << getReadableTime(getTimeStampAfter(ocrl->get(i)->getModificationTime(), "15d")) << ")";
                            }
                        }

                        delete ocrl;
                    }
                    LOG_err << os.str();
                }
                delete megaCmdListener;
            }
        }

        return;
    }
    else if (words[0] == "signup")
    {
        if (api->isLoggedIn())
        {
            setCurrentOutCode(MCMD_INVALIDSTATE);
            LOG_err << "Please loggout first ";
        }
        else if (words.size() > 1)
        {
            string email = words[1];
            if (words.size() > 2)
            {
                string name = getOption(cloptions, "name", email);
                string passwd = words[2];
                signup(name, passwd, email);
            }
            else
            {
                login = words[1];
                name = getOption(cloptions, "name", email);
                signingup = true;
                if (interactiveThread())
                {
                    setprompt(NEWPASSWORD);
                }
                else
                {
                    setCurrentOutCode(MCMD_EARGS);
                    LOG_err << "Extra args required in non interactive mode. Usage: " << getUsageStr("signup");
                }
            }
        }
        else
        {
            setCurrentOutCode(MCMD_EARGS);
            LOG_err << "      " << getUsageStr("signup");
        }

        return;
    }
    else if (words[0] == "whoami")
    {
        MegaUser *u = api->getMyUser();
        if (u)
        {
            OUTSTREAM << "Account e-mail: " << u->getEmail() << std::endl;
            if (getFlag(clflags, "l"))
            {
                MegaCmdListener *megaCmdListener = new MegaCmdListener(NULL);
                api->getExtendedAccountDetails(true, true, true, megaCmdListener);
                actUponGetExtendedAccountDetails(megaCmdListener);
                delete megaCmdListener;
            }
            delete u;
        }
        else
        {
            setCurrentOutCode(MCMD_NOTLOGGEDIN);
            LOG_err << "Not logged in.";
        }

        return;
    }
    else if (words[0] == "export")
    {
        if (!api->isFilesystemAvailable())
        {
            setCurrentOutCode(MCMD_NOTLOGGEDIN);
            LOG_err << "Not logged in.";
            return;
        }
        time_t expireTime = 0;
        string sexpireTime = getOption(cloptions, "expire", "");
        if ("" != sexpireTime)
        {
            expireTime = getTimeStampAfter(sexpireTime);
        }
        if (expireTime < 0)
        {
            setCurrentOutCode(MCMD_EARGS);
            LOG_err << "Invalid time " << sexpireTime;
            return;
        }

        if (words.size() <= 1)
        {
            words.push_back(string(".")); //cwd
        }

        for (int i = 1; i < (int)words.size(); i++)
        {
            unescapeifRequired(words[i]);
            if (isRegExp(words[i]))
            {
                vector<MegaNode *> *nodes = nodesbypath(words[i].c_str(), getFlag(clflags,"use-pcre"));
                if (nodes)
                {
                    if (!nodes->size())
                    {
                        setCurrentOutCode(MCMD_NOTFOUND);
                        LOG_err << "Nodes not found: " << words[i];
                    }
                    for (std::vector< MegaNode * >::iterator it = nodes->begin(); it != nodes->end(); ++it)
                    {
                        MegaNode * n = *it;
                        if (n)
                        {
                            if (getFlag(clflags, "a"))
                            {
                                LOG_debug << " exporting ... " << n->getName() << " expireTime=" << expireTime;
                                exportNode(n, expireTime, getFlag(clflags,"f"));
                            }
                            else if (getFlag(clflags, "d"))
                            {
                                LOG_debug << " deleting export ... " << n->getName();
                                disableExport(n);
                            }
                            else
                            {
                                if (dumpListOfExported(n, words[i]) == 0 )
                                {
                                    OUTSTREAM << words[i] << " is not exported. Use -a to export it" << std::endl;
                                }
                            }
                            delete n;
                        }
                    }

                    nodes->clear();
                    delete nodes;
                }
                else
                {
                    setCurrentOutCode(MCMD_NOTFOUND);
                    LOG_err << "Node not found: " << words[i];
                }
            }
            else
            {
                MegaNode *n = nodebypath(words[i].c_str());
                if (n)
                {
                    if (getFlag(clflags, "a"))
                    {
                        LOG_debug << " exporting ... " << n->getName();
                        exportNode(n, expireTime, getFlag(clflags,"f"));
                    }
                    else if (getFlag(clflags, "d"))
                    {
                        LOG_debug << " deleting export ... " << n->getName();
                        disableExport(n);
                    }
                    else
                    {
                        if (dumpListOfExported(n, words[i]) == 0 )
                        {
                            OUTSTREAM << "Couldn't find anything exported below ";
                            if (words[i] == ".")
                            {
                                OUTSTREAM << "current folder";
                            }
                            else
                            {
                                OUTSTREAM << "<";
                                OUTSTREAM << words[i];
                                OUTSTREAM << ">";
                            }
                            OUTSTREAM << ". Use -a to export " << (words[i].size()?"it":"something") << std::endl;
                        }
                    }
                    delete n;
                }
                else
                {
                    setCurrentOutCode(MCMD_NOTFOUND);
                    LOG_err << "Node not found: " << words[i];
                }
            }
        }

        return;
    }
    else if (words[0] == "import")
    {
        if (!api->isFilesystemAvailable())
        {
            setCurrentOutCode(MCMD_NOTLOGGEDIN);
            LOG_err << "Not logged in.";
            return;
        }
        string remotePath = "";
        MegaNode *dstFolder = NULL;
        if (words.size() > 1) //link
        {
            if (isPublicLink(words[1]))
            {
                if (words.size() > 2)
                {
                    remotePath = words[2];
                    dstFolder = nodebypath(remotePath.c_str());
                }
                else
                {
                    dstFolder = api->getNodeByHandle(cwd);
                    remotePath = "."; //just to inform (alt: getpathbynode)
                }
                if (dstFolder && ( !dstFolder->getType() == MegaNode::TYPE_FILE ))
                {
                    if (getLinkType(words[1]) == MegaNode::TYPE_FILE)
                    {
                        MegaCmdListener *megaCmdListener = new MegaCmdListener(NULL);

                        api->importFileLink(words[1].c_str(), dstFolder, megaCmdListener);
                        megaCmdListener->wait();
                        if (checkNoErrors(megaCmdListener->getError(), "import node"))
                        {
                            MegaNode *imported = api->getNodeByHandle(megaCmdListener->getRequest()->getNodeHandle());
                            char *importedPath = api->getNodePath(imported);
                            LOG_info << "Import file complete: " << importedPath;
                            delete imported;
                            delete []importedPath;
                        }

                        delete megaCmdListener;
                    }
                    else if (getLinkType(words[1]) == MegaNode::TYPE_FOLDER)
                    {
                        MegaApi* apiFolder = getFreeApiFolder();
                        char *accountAuth = api->getAccountAuth();
                        apiFolder->setAccountAuth(accountAuth);
                        delete []accountAuth;

                        MegaCmdListener *megaCmdListener = new MegaCmdListener(apiFolder, NULL);
                        apiFolder->loginToFolder(words[1].c_str(), megaCmdListener);
                        megaCmdListener->wait();
                        if (checkNoErrors(megaCmdListener->getError(), "login to folder"))
                        {
                            MegaCmdListener *megaCmdListener2 = new MegaCmdListener(apiFolder, NULL);
                            apiFolder->fetchNodes(megaCmdListener2);
                            megaCmdListener2->wait();
                            if (checkNoErrors(megaCmdListener2->getError(), "access folder link " + words[1]))
                            {
                                MegaNode *folderRootNode = apiFolder->getRootNode();
                                if (folderRootNode)
                                {
                                    MegaNode *authorizedNode = apiFolder->authorizeNode(folderRootNode);
                                    if (authorizedNode != NULL)
                                    {
                                        MegaCmdListener *megaCmdListener3 = new MegaCmdListener(apiFolder, NULL);
                                        api->copyNode(authorizedNode, dstFolder, megaCmdListener3);
                                        megaCmdListener3->wait();
                                        if (checkNoErrors(megaCmdListener->getError(), "import folder node"))
                                        {
                                            MegaNode *importedFolderNode = api->getNodeByHandle(megaCmdListener3->getRequest()->getNodeHandle());
                                            char *pathnewFolder = api->getNodePath(importedFolderNode);
                                            if (pathnewFolder)
                                            {
                                                OUTSTREAM << "Imported folder complete: " << pathnewFolder << std::endl;
                                                delete []pathnewFolder;
                                            }
                                            delete importedFolderNode;
                                        }
                                        delete megaCmdListener3;
                                        delete authorizedNode;
                                    }
                                    else
                                    {
                                        setCurrentOutCode(MCMD_EUNEXPECTED);
                                        LOG_debug << "Node couldn't be authorized: " << words[1];
                                    }
                                    delete folderRootNode;
                                }
                                else
                                {
                                    setCurrentOutCode(MCMD_INVALIDSTATE);
                                    LOG_err << "Couldn't get root folder for folder link";
                                }
                            }
                            delete megaCmdListener2;
                        }
                        delete megaCmdListener;
                        freeApiFolder(apiFolder);
                    }
                    else
                    {
                        setCurrentOutCode(MCMD_EARGS);
                        LOG_err << "Invalid link: " << words[1];
                        LOG_err << "      " << getUsageStr("import");
                    }
                }
                else
                {
                    setCurrentOutCode(MCMD_INVALIDTYPE);
                    LOG_err << "Invalid destiny: " << remotePath;
                }
                delete dstFolder;
            }
            else
            {
                setCurrentOutCode(MCMD_INVALIDTYPE);
                LOG_err << "Invalid link: " << words[1];
            }
        }
        else
        {
            setCurrentOutCode(MCMD_EARGS);
            LOG_err << "      " << getUsageStr("import");
        }

        return;
    }
    else if (words[0] == "reload")
    {
        int clientID = getintOption(cloptions, "clientID", -1);

        OUTSTREAM << "Reloading account..." << std::endl;
        MegaCmdListener *megaCmdListener = new MegaCmdListener(NULL, NULL, clientID);
        api->fetchNodes(megaCmdListener);
        actUponFetchNodes(api, megaCmdListener);
        delete megaCmdListener;
        return;
    }
    else if (words[0] == "logout")
    {
        OUTSTREAM << "Logging out..." << std::endl;
        MegaCmdListener *megaCmdListener = new MegaCmdListener(NULL);
        bool keepSession = getFlag(clflags, "keep-session");
        char * dumpSession = NULL;

        if (keepSession) //local logout
        {
            dumpSession = api->dumpSession();
            api->localLogout(megaCmdListener);
        }
        else
        {
            api->logout(megaCmdListener);
        }
        actUponLogout(megaCmdListener, keepSession);
        if (keepSession)
        {
            OUTSTREAM << "Session closed but not deleted. Warning: it will be restored the next time you execute the application. Execute \"logout\" to delete the session permanently." << std::endl;

            if (dumpSession)
            {
                OUTSTREAM << "You can also login with the session id: " << dumpSession << std::endl;
                delete []dumpSession;
            }
        }
        delete megaCmdListener;

        return;
    }
    else if (words[0] == "confirm")
    {
        if (words.size() > 2)
        {
            string link = words[1];
            string email = words[2];
            // check email corresponds with link:
            MegaCmdListener *megaCmdListener = new MegaCmdListener(NULL);
            api->querySignupLink(link.c_str(), megaCmdListener);
            megaCmdListener->wait();
            if (checkNoErrors(megaCmdListener->getError(), "check email corresponds to link"))
            {
                if (megaCmdListener->getRequest()->getEmail() && email == megaCmdListener->getRequest()->getEmail())
                {
                    string passwd;
                    if (words.size() > 3)
                    {
                        passwd = words[3];
                        confirm(passwd, email, link);
                    }
                    else
                    {
                        this->login = email;
                        this->link = link;
                        confirming = true;
                        if (interactiveThread() && !getCurrentThreadIsCmdShell())
                        {
                            setprompt(LOGINPASSWORD);
                        }
                        else
                        {
                            setCurrentOutCode(MCMD_EARGS);
                            LOG_err << "Extra args required in non interactive mode. Usage: " << getUsageStr("confirm");
                        }
                    }
                }
                else
                {
                    setCurrentOutCode(MCMD_INVALIDEMAIL);
                    LOG_err << email << " doesn't correspond to the confirmation link: " << link;
                }
            }

            delete megaCmdListener;
        }
        else
        {
            setCurrentOutCode(MCMD_EARGS);
            LOG_err << "      " << getUsageStr("confirm");
        }

        return;
    }
    else if (words[0] == "session")
    {
        char * dumpSession = api->dumpSession();
        if (dumpSession)
        {
            OUTSTREAM << "Your (secret) session is: " << dumpSession << std::endl;
            delete []dumpSession;
        }
        else
        {
            setCurrentOutCode(MCMD_NOTLOGGEDIN);
            LOG_err << "Not logged in.";
        }
        return;
    }
    else if (words[0] == "history")
    {
        return;
    }
    else if (words[0] == "version")
    {
        OUTSTREAM << "MEGA CMD version: " << MEGACMD_MAJOR_VERSION << "." << MEGACMD_MINOR_VERSION << "." << MEGACMD_MICRO_VERSION << ": code " << MEGACMD_CODE_VERSION << std::endl;

        MegaCmdListener *megaCmdListener = new MegaCmdListener(NULL);
        api->getLastAvailableVersion("BdARkQSQ",megaCmdListener);
        if (!megaCmdListener->trywait(2000))
        {
            if (!megaCmdListener->getError())
            {
                LOG_fatal << "No MegaError at getLastAvailableVersion: ";
            }
            else if (megaCmdListener->getError()->getErrorCode() != MegaError::API_OK)
            {
                LOG_debug << "Couldn't get latests available version: " << megaCmdListener->getError()->getErrorString();
            }
            else
            {
                if (megaCmdListener->getRequest()->getNumber() != MEGACMD_CODE_VERSION)
                {
                    OUTSTREAM << "---------------------------------------------------------------------" << std::endl;
                    OUTSTREAM << "--        There is a new version available of megacmd: " << std::setw(12) << std::left << megaCmdListener->getRequest()->getName() << "--" << std::endl;
                    OUTSTREAM << "--        Please, download it from https://mega.nz/cmd             --" << std::endl;
#if defined(__APPLE__)
                    OUTSTREAM << "--        Before installing enter \"exit\" to close MEGAcmd          --" << std::endl;
#endif
                    OUTSTREAM << "---------------------------------------------------------------------" << std::endl;
                }
            }
            delete megaCmdListener;
        }
        else
        {
            LOG_debug << "Couldn't get latests available version (petition timed out)";

            api->removeRequestListener(megaCmdListener);
            delete megaCmdListener;
        }





        if (getFlag(clflags,"c"))
        {
            OUTSTREAM << "Changes in the current version:" << std::endl;
            string thechangelog = megacmdchangelog;
            if (thechangelog.size())
            {
                replaceAll(thechangelog,"\n","\n * ");
                OUTSTREAM << " * " << thechangelog << std::endl << std::endl;
            }
        }
        if (getFlag(clflags,"l"))
        {
            OUTSTREAM << "MEGA SDK version: " << MEGA_MAJOR_VERSION << "." << MEGA_MINOR_VERSION << "." << MEGA_MICRO_VERSION << std::endl;

            OUTSTREAM << "MEGA SDK Credits: https://github.com/meganz/sdk/blob/master/CREDITS.md" << std::endl;
            OUTSTREAM << "MEGA SDK License: https://github.com/meganz/sdk/blob/master/LICENSE" << std::endl;
            OUTSTREAM << "MEGAcmd License: https://github.com/meganz/megacmd/blob/master/LICENSE" << std::endl;

            OUTSTREAM << "Features enabled:" << std::endl;

#ifdef USE_CRYPTOPP
            OUTSTREAM << "* CryptoPP" << std::endl;
#endif

#ifdef USE_SQLITE
          OUTSTREAM << "* SQLite" << std::endl;
#endif

#ifdef USE_BDB
            OUTSTREAM << "* Berkeley DB" << std::endl;
#endif

#ifdef USE_INOTIFY
           OUTSTREAM << "* inotify" << std::endl;
#endif

#ifdef HAVE_FDOPENDIR
           OUTSTREAM << "* fdopendir" << std::endl;
#endif

#ifdef HAVE_SENDFILE
            OUTSTREAM << "* sendfile" << std::endl;
#endif

#ifdef _LARGE_FILES
           OUTSTREAM << "* _LARGE_FILES" << std::endl;
#endif

#ifdef USE_FREEIMAGE
            OUTSTREAM << "* FreeImage" << std::endl;
#endif

#ifdef USE_PCRE
            OUTSTREAM << "* PCRE" << std::endl;
#endif

#ifdef ENABLE_SYNC
           OUTSTREAM << "* sync subsystem" << std::endl;
#endif
        }
        return;
    }
    else if (words[0] == "masterkey")
    {
        if (!api->isFilesystemAvailable())
        {
            setCurrentOutCode(MCMD_NOTLOGGEDIN);
            LOG_err << "Not logged in.";
            return;
        }
        OUTSTREAM << api->exportMasterKey() << std::endl;
        api->masterKeyExported(); //we do not wait for this to end
    }
    else if (words[0] == "showpcr")
    {
        if (!api->isFilesystemAvailable())
        {
            setCurrentOutCode(MCMD_NOTLOGGEDIN);
            LOG_err << "Not logged in.";
            return;
        }
        bool incoming = getFlag(clflags, "in");
        bool outgoing = getFlag(clflags, "out");

        if (!incoming && !outgoing)
        {
            incoming = true;
            outgoing = true;
        }

        if (outgoing)
        {
            MegaContactRequestList *ocrl = api->getOutgoingContactRequests();
            if (ocrl)
            {
                if (ocrl->size())
                {
                    OUTSTREAM << "Outgoing PCRs:" << std::endl;
                }
                for (int i = 0; i < ocrl->size(); i++)
                {
                    MegaContactRequest * cr = ocrl->get(i);
                    OUTSTREAM << " " << std::setw(22) << cr->getTargetEmail();

                    char * sid = api->userHandleToBase64(cr->getHandle());

                    OUTSTREAM << "\t (id: " << sid << ", creation: " << getReadableTime(cr->getCreationTime())
                              << ", modification: " << getReadableTime(cr->getModificationTime()) << ")";

                    delete[] sid;
                    OUTSTREAM << std::endl;
                }

                delete ocrl;
            }
        }

        if (incoming)
        {
            MegaContactRequestList *icrl = api->getIncomingContactRequests();
            if (icrl)
            {
                if (icrl->size())
                {
                    OUTSTREAM << "Incoming PCRs:" << std::endl;
                }

                for (int i = 0; i < icrl->size(); i++)
                {
                    MegaContactRequest * cr = icrl->get(i);
                    OUTSTREAM << " " << std::setw(22) << cr->getSourceEmail();

                    MegaHandle id = cr->getHandle();
                    char sid[12];
                    Base64::btoa((byte*)&( id ), sizeof( id ), sid);

                    OUTSTREAM << "\t (id: " << sid << ", creation: " << getReadableTime(cr->getCreationTime())
                              << ", modification: " << getReadableTime(cr->getModificationTime()) << ")";
                    if (cr->getSourceMessage())
                    {
                        OUTSTREAM << std::endl << "\t" << "Invitation message: " << cr->getSourceMessage();
                    }

                    OUTSTREAM << std::endl;
                }

                delete icrl;
            }
        }
        return;
    }
    else if (words[0] == "killsession")
    {
        string thesession;
        MegaHandle thehandle = UNDEF;
        if (getFlag(clflags, "a"))
        {
            // Kill all sessions (except current)
            thesession = "all";
            thehandle = INVALID_HANDLE;
        }
        else if (words.size() > 1)
        {
            thesession = words[1];
            thehandle = api->base64ToUserHandle(thesession.c_str());
        }
        else
        {
            setCurrentOutCode(MCMD_EARGS);
            LOG_err << "      " << getUsageStr("killsession");
            return;
        }

        MegaCmdListener *megaCmdListener = new MegaCmdListener(NULL);
        api->killSession(thehandle, megaCmdListener);
        megaCmdListener->wait();
        if (checkNoErrors(megaCmdListener->getError(), "kill session " + thesession + ". Maybe the session was not valid."))
        {
            if (!getFlag(clflags, "a"))
            {
               OUTSTREAM << "Session " << thesession << " killed successfully" << std::endl;
            }
            else
            {
                OUTSTREAM << "All sessions killed successfully" << std::endl;
            }
        }

        delete megaCmdListener;
        return;
    }
    else if (words[0] == "transfers")
    {
        bool showcompleted = getFlag(clflags, "show-completed");
        bool onlycompleted = getFlag(clflags, "only-completed");
        bool onlyuploads = getFlag(clflags, "only-uploads");
        bool onlydownloads = getFlag(clflags, "only-downloads");
        bool showsyncs = getFlag(clflags, "show-syncs");

        int PATHSIZE = getintOption(cloptions,"path-display-size");
        if (!PATHSIZE)
        {
            // get screen size for output purposes
            unsigned int width = getNumberOfCols(75);
            PATHSIZE = std::min(60,int((width-46)/2));
        }

        if (getFlag(clflags,"c"))
        {
            if (getFlag(clflags,"a"))
            {
                if (onlydownloads || (!onlyuploads && !onlydownloads) )
                {
                    MegaCmdListener *megaCmdListener = new MegaCmdListener(NULL);
                    api->cancelTransfers(MegaTransfer::TYPE_DOWNLOAD, megaCmdListener);
                    megaCmdListener->wait();
                    if (checkNoErrors(megaCmdListener->getError(), "cancel all download transfers"))
                    {
                        OUTSTREAM << "Download transfers cancelled successfully." << std::endl;
                    }
                    delete megaCmdListener;
                }
                if (onlyuploads || (!onlyuploads && !onlydownloads) )
                {
                    MegaCmdListener *megaCmdListener = new MegaCmdListener(NULL);
                    api->cancelTransfers(MegaTransfer::TYPE_UPLOAD, megaCmdListener);
                    megaCmdListener->wait();
                    if (checkNoErrors(megaCmdListener->getError(), "cancel all upload transfers"))
                    {
                        OUTSTREAM << "Upload transfers cancelled successfully." << std::endl;
                    }
                    delete megaCmdListener;
                }

            }
            else
            {
                if (words.size() < 2)
                {
                    setCurrentOutCode(MCMD_EARGS);
                    LOG_err << "      " << getUsageStr("transfers");
                    return;
                }
                for (unsigned int i = 1; i < words.size(); i++)
                {
                    MegaTransfer *transfer = api->getTransferByTag(toInteger(words[i],-1));
                    if (transfer)
                    {
                        if (transfer->isSyncTransfer())
                        {
                            LOG_err << "Unable to cancel transfer with tag " << words[i] << ". Sync transfers cannot be cancelled";
                            setCurrentOutCode(MCMD_INVALIDTYPE);
                        }
                        else
                        {
                            MegaCmdListener *megaCmdListener = new MegaCmdListener(NULL);
                            api->cancelTransfer(transfer, megaCmdListener);
                            megaCmdListener->wait();
                            if (checkNoErrors(megaCmdListener->getError(), "cancel transfer with tag " + words[i] + "."))
                            {
                                OUTSTREAM << "Transfer " << words[i]<< " cancelled successfully." << std::endl;
                            }
                            delete megaCmdListener;
                        }
                    }
                    else
                    {
                        LOG_err << "Coul not find transfer with tag: " << words[i];
                        setCurrentOutCode(MCMD_NOTFOUND);
                    }
                }
            }

            return;
        }

        if (getFlag(clflags,"p") || getFlag(clflags,"r"))
        {
            if (getFlag(clflags,"a"))
            {
                if (onlydownloads || (!onlyuploads && !onlydownloads) )
                {
                    MegaCmdListener *megaCmdListener = new MegaCmdListener(NULL);
                    api->pauseTransfers(getFlag(clflags,"p"), MegaTransfer::TYPE_DOWNLOAD, megaCmdListener);
                    megaCmdListener->wait();
                    if (checkNoErrors(megaCmdListener->getError(), (getFlag(clflags,"p")?"pause all download transfers":"resume all download transfers")))
                    {
                        OUTSTREAM << "Download transfers "<< (getFlag(clflags,"p")?"pause":"resume") << "d successfully." << std::endl;
                    }
                    delete megaCmdListener;
                }
                if (onlyuploads || (!onlyuploads && !onlydownloads) )
                {
                    MegaCmdListener *megaCmdListener = new MegaCmdListener(NULL);
                    api->pauseTransfers(getFlag(clflags,"p"), MegaTransfer::TYPE_UPLOAD, megaCmdListener);
                    megaCmdListener->wait();
                    if (checkNoErrors(megaCmdListener->getError(), (getFlag(clflags,"p")?"pause all download transfers":"resume all download transfers")))
                    {
                        OUTSTREAM << "Upload transfers "<< (getFlag(clflags,"p")?"pause":"resume") << "d successfully." << std::endl;
                    }
                    delete megaCmdListener;
                }

            }
            else
            {
                if (words.size() < 2)
                {
                    setCurrentOutCode(MCMD_EARGS);
                    LOG_err << "      " << getUsageStr("transfers");
                    return;
                }
                for (unsigned int i = 1; i < words.size(); i++)
                {
                    MegaTransfer *transfer = api->getTransferByTag(toInteger(words[i],-1));
                    if (transfer)
                    {
                        if (transfer->isSyncTransfer())
                        {
                            LOG_err << "Unable to "<< (getFlag(clflags,"p")?"pause":"resume") << " transfer with tag " << words[i] << ". Sync transfers cannot be "<< (getFlag(clflags,"p")?"pause":"resume") << "d";
                            setCurrentOutCode(MCMD_INVALIDTYPE);
                        }
                        else
                        {
                            MegaCmdListener *megaCmdListener = new MegaCmdListener(NULL);
                            api->pauseTransfer(transfer, getFlag(clflags,"p"), megaCmdListener);
                            megaCmdListener->wait();
                            if (checkNoErrors(megaCmdListener->getError(), (getFlag(clflags,"p")?"pause transfer with tag ":"resume transfer with tag ") + words[i] + "."))
                            {
                                OUTSTREAM << "Transfer " << words[i]<< " "<< (getFlag(clflags,"p")?"pause":"resume") << "d successfully." << std::endl;
                            }
                            delete megaCmdListener;
                        }
                    }
                    else
                    {
                        LOG_err << "Coul not find transfer with tag: " << words[i];
                        setCurrentOutCode(MCMD_NOTFOUND);
                    }
                }
            }

            return;
        }

        //show transfers
        MegaTransferData* transferdata = api->getTransferData();
        int limit = getintOption(cloptions, "limit", std::min(10,transferdata->getNumDownloads()+transferdata->getNumUploads()+(int)globalTransferListener->completedTransfers.size()));

        if (!transferdata)
        {
            setCurrentOutCode(MCMD_EUNEXPECTED);
            LOG_err << "No transferdata.";
            return;
        }

        bool downloadpaused = api->areTransfersPaused(MegaTransfer::TYPE_DOWNLOAD);
        bool uploadpaused = api->areTransfersPaused(MegaTransfer::TYPE_UPLOAD);

        int indexUpload = 0;
        int indexDownload = 0;
        int shown = 0;

        int showndl = 0;
        int shownup = 0;
        unsigned int shownCompleted = 0;

        vector<MegaTransfer *> transfersDLToShow;
        vector<MegaTransfer *> transfersUPToShow;
        vector<MegaTransfer *> transfersCompletedToShow;

        if (showcompleted)
        {
            globalTransferListener->completedTransfersMutex.lock();
            size_t totalcompleted = globalTransferListener->completedTransfers.size();
            for (size_t i = 0;(i < totalcompleted)
                 && (shownCompleted < totalcompleted)
                 && (shownCompleted < (size_t)(limit+1)); //Note limit+1 to seek for one more to show if there are more to show!
                 i++)
            {
                MegaTransfer *transfer = globalTransferListener->completedTransfers.at(i);
                if (
                    (
                            (transfer->getType() == MegaTransfer::TYPE_UPLOAD && (onlyuploads || (!onlyuploads && !onlydownloads) ))
                        ||  (transfer->getType() == MegaTransfer::TYPE_DOWNLOAD && (onlydownloads || (!onlyuploads && !onlydownloads) ) )
                    )
                    &&  !(!showsyncs && transfer->isSyncTransfer())
                    )
                {

                    transfersCompletedToShow.push_back(transfer);
                    shownCompleted++;
                }
            }
            globalTransferListener->completedTransfersMutex.unlock();
        }

        shown += shownCompleted;

        while (!onlycompleted)
        {
            //MegaTransfer *transfer = transferdata->get(i);
            MegaTransfer *transfer = NULL;
            //Next transfer to show
            if (onlyuploads && !onlydownloads && indexUpload < transferdata->getNumUploads()) //Only uploads
            {
                transfer = api->getTransferByTag(transferdata->getUploadTag(indexUpload++));
            }
            else
            {
                if ( (!onlydownloads || (onlydownloads && onlyuploads)) //both
                     && ( (shown >= (limit/2) ) || indexDownload == transferdata->getNumDownloads() ) // /already chosen half slots for dls or no more dls
                     && indexUpload < transferdata->getNumUploads()
                     )
                    //This is not 100 % perfect, it could show with a limit of 10 5 downloads and 3 uploads with more downloads on the queue.
                {
                    transfer = api->getTransferByTag(transferdata->getUploadTag(indexUpload++));

                }
                else if(indexDownload < transferdata->getNumDownloads())
                {
                    transfer =  api->getTransferByTag(transferdata->getDownloadTag(indexDownload++));
                }
            }

            if (!transfer) break; //finish

            if (
                    (showcompleted || transfer->getState() != MegaTransfer::STATE_COMPLETED)
                    &&  !(onlyuploads && transfer->getType() != MegaTransfer::TYPE_UPLOAD && !onlydownloads )
                    &&  !(onlydownloads && transfer->getType() != MegaTransfer::TYPE_DOWNLOAD && !onlyuploads )
                    &&  !(!showsyncs && transfer->isSyncTransfer())
                    &&  (shown < (limit+1)) //Note limit+1 to seek for one more to show if there are more to show!
                    )
            {
                shown++;
                if (transfer->getType() == MegaTransfer::TYPE_DOWNLOAD)
                {
                    transfersDLToShow.push_back(transfer);
                    showndl++;
                }
                else
                {
                    transfersUPToShow.push_back(transfer);
                    shownup++;
                }
            }
            else
            {
                delete transfer;
            }
            if (shown>limit || transfer == NULL) //we-re done
            {
                break;
            }
        }

        delete transferdata;

        vector<MegaTransfer *>::iterator itCompleted = transfersCompletedToShow.begin();
        vector<MegaTransfer *>::iterator itDLs = transfersDLToShow.begin();
        vector<MegaTransfer *>::iterator itUPs = transfersUPToShow.begin();

        for (unsigned int i=0;i<showndl+shownup+shownCompleted; i++)
        {
            MegaTransfer *transfer = NULL;
            bool deleteTransfer = true;
            if (itDLs == transfersDLToShow.end() && itCompleted == transfersCompletedToShow.end())
            {
                transfer = (MegaTransfer *) *itUPs;
                itUPs++;
            }
            else if (itCompleted == transfersCompletedToShow.end())
            {
                transfer = (MegaTransfer *) *itDLs;
                itDLs++;
            }
            else
            {
                transfer = (MegaTransfer *) *itCompleted;
                itCompleted++;
                deleteTransfer=false;
            }
            if (i == 0) //first
            {
                if (uploadpaused || downloadpaused)
                {
                    OUTSTREAM << "            " << (downloadpaused?"DOWNLOADS":"") << ((uploadpaused && downloadpaused)?" AND ":"")
                              << (uploadpaused?"UPLOADS":"") << " ARE PAUSED " << std::endl;
                }
                printTransfersHeader(PATHSIZE);
            }
            if (i==(unsigned int)limit) //we are in the extra one (not to be shown)
            {
                OUTSTREAM << " ...  Showing first " << limit << " transfers ..." << std::endl;
                if (deleteTransfer)
                {
                    delete transfer;
                }
                break;
            }

            printTransfer(transfer, PATHSIZE);

            if (deleteTransfer)
            {
                delete transfer;
            }
        }
    }
    else if (words[0] == "locallogout")
    {
        OUTSTREAM << "Logging out locally..." << std::endl;
        cwd = UNDEF;
        return;
    }
    else
    {
        setCurrentOutCode(MCMD_EARGS);
        LOG_err << "Invalid command: " << words[0];
    }
}

bool MegaCmdExecuter::checkNoErrors(MegaError *error, string message)
{
    if (!error)
    {
        LOG_fatal << "No MegaError at request: " << message;
        return false;
    }
    if (error->getErrorCode() == MegaError::API_OK)
    {
        return true;
    }

    setCurrentOutCode(error->getErrorCode());
    LOG_err << "Failed to " << message << ": " << error->getErrorString();
    return false;
}
