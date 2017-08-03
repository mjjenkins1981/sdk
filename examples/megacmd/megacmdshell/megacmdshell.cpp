/**
 * @file examples/megacmd/megacmdshell.cpp
 * @brief MEGAcmd: Interactive CLI and service application
 * This is the shell application
 *
 * (c) 2013-2017 by Mega Limited, Auckland, New Zealand
 *
 * This file is distributed under the terms of the GNU General Public
 * License, see http://www.gnu.org/copyleft/gpl.txt
 * for details.
 *
 * This file is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 */

#include "megacmdshell.h"
#include "megacmdshellcommunications.h"
#include "megacmdshellcommunicationsnamedpipes.h"

#define USE_VARARGS
#define PREFER_STDARG

#include <readline/readline.h>
#include <readline/history.h>

#include <iomanip>
#include <string>
#include <set>
#include <map>
#include <vector>
#include <sstream>
#include <algorithm>
#include <stdio.h>


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

#ifndef _WIN32
#include <signal.h>
#include <sys/types.h>
#include <errno.h>
#endif

#if defined(_WIN32) || defined(_WIN64)
  #define snprintf _snprintf
  #define vsnprintf _vsnprintf
  #define strcasecmp _stricmp
  #define strncasecmp _strnicmp
#endif

#define SSTR( x ) static_cast< std::ostringstream & >( \
        ( std::ostringstream() << std::dec << x ) ).str()

#if defined(_WIN32) && !defined(WINDOWS_PHONE)
#include "mega/thread/win32thread.h"
class MegaMutex : public mega::Win32Mutex {};
#elif defined(USE_CPPTHREAD)
#include "mega/thread/cppthread.h"
class MegaMutex : public mega::CppMutex {};
#else
#include "mega/thread/posixthread.h"
class MegaMutex : public mega::PosixMutex {};
#endif


using namespace std;


// utility functions
char * dupstr(char* s)
{
    char *r;

    r = (char*)malloc(sizeof( char ) * ( strlen(s) + 1 ));
    strcpy(r, s);
    return( r );
}

void replaceAll(std::string& str, const std::string& from, const std::string& to)
{
    if (from.empty())
    {
        return;
    }
    size_t start_pos = 0;
    while (( start_pos = str.find(from, start_pos)) != std::string::npos)
    {
        str.replace(start_pos, from.length(), to);
        start_pos += to.length();
    }
}

string getCurrentLine()
{
    char *saved_line = rl_copy_text(0, rl_point);
    string toret(saved_line);
    free(saved_line);
    saved_line = NULL;
    return toret;
}

void sleepSeconds(int seconds)
{
#ifdef _WIN32
    Sleep(1000*seconds);
#else
    sleep(seconds);
#endif
}

void sleepMicroSeconds(long microseconds)
{
#ifdef _WIN32
    Sleep(microseconds);
#else
    usleep(microseconds*1000);
#endif
}

// end utily functions

void statechangehandle(string newstate)
{
    if (newstate.compare(0, strlen("prompt:"), "prompt:") == 0)
    {
        changeprompt(newstate.substr(strlen("prompt:")).c_str(),true);
    }
    else if (newstate == "ack")
    {
        // do nothing, all good
    }
    else
    {
        cerr << "received unrecognized state change: " << newstate << endl;
        //sleep a while to avoid continuous looping
        sleepSeconds(1);
    }
}

// Console related functions:
void console_readpwchar(char* pw_buf, int pw_buf_size, int* pw_buf_pos, char** line)
{
#ifdef _WIN32
    char c;
      DWORD cread;

      if (ReadConsole(GetStdHandle(STD_INPUT_HANDLE), &c, 1, &cread, NULL) == 1)
      {
          if ((c == 8) && *pw_buf_pos)
          {
              (*pw_buf_pos)--;
          }
          else if (c == 13)
          {
              *line = (char*)malloc(*pw_buf_pos + 1);
              memcpy(*line, pw_buf, *pw_buf_pos);
              (*line)[*pw_buf_pos] = 0;
          }
          else if (*pw_buf_pos < pw_buf_size)
          {
              pw_buf[(*pw_buf_pos)++] = c;
          }
      }
#else
    // FIXME: UTF-8 compatibility

    char c;

    if (read(STDIN_FILENO, &c, 1) == 1)
    {
        if (c == 8 && *pw_buf_pos)
        {
            (*pw_buf_pos)--;
        }
        else if (c == 13)
        {
            *line = (char*) malloc(*pw_buf_pos + 1);
            memcpy(*line, pw_buf, *pw_buf_pos);
            (*line)[*pw_buf_pos] = 0;
        }
        else if (*pw_buf_pos < pw_buf_size)
        {
            pw_buf[(*pw_buf_pos)++] = c;
        }
    }
#endif
}
void console_setecho(bool echo)
{
#ifdef _WIN32
    HANDLE hCon = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode;

    GetConsoleMode(hCon, &mode);

    if (echo)
    {
        mode |= ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT;
    }
    else
    {
        mode &= ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT);
    }

    SetConsoleMode(hCon, mode);
#else
    //do nth
#endif
}

int getNumberOfCols(u_int defaultwidth=0)
{
    u_int width = defaultwidth;
    int rows = 1, cols = width;
#if defined( RL_ISSTATE ) && defined( RL_STATE_INITIALIZED )

    if (RL_ISSTATE(RL_STATE_INITIALIZED))
    {
        rl_resize_terminal();
        rl_get_screen_size(&rows, &cols);
    }
#endif

    if (cols)
    {
        width = cols-2;
#ifdef _WIN32
        width--;
#endif
    }
    return width;
}

// password change-related state information
string oldpasswd;
string newpasswd;

bool doExit = false;

bool handlerinstalled = false;

bool requirepromptinstall = true;

bool procesingline = false;

static char dynamicprompt[128];

static char* line;

static prompttype prompt = COMMAND;

static char pw_buf[256];
static int pw_buf_pos;

string loginname;
string linktoconfirm;

bool confirminglink = false;

// communications with megacmdserver:
MegaCmdShellCommunications *comms;

MegaMutex mutexPrompt;

void printWelcomeMsg(u_int width = 0);

void sigint_handler(int signum)
{
    if (prompt != COMMAND)
    {
        setprompt(COMMAND);
    }

    // reset position and print prompt
    rl_replace_line("", 0); //clean contents of actual command
    rl_crlf(); //move to nextline

    if (RL_ISSTATE(RL_STATE_ISEARCH) || RL_ISSTATE(RL_STATE_ISEARCH) || RL_ISSTATE(RL_STATE_ISEARCH))
    {
        RL_UNSETSTATE(RL_STATE_ISEARCH);
        RL_UNSETSTATE(RL_STATE_NSEARCH);
        RL_UNSETSTATE( RL_STATE_SEARCH);
        history_set_pos(history_length);
        rl_restore_prompt(); // readline has stored it when searching
    }
    else
    {
        rl_reset_line_state();
    }
    rl_redisplay();
}

#ifdef _WIN32
BOOL CtrlHandler( DWORD fdwCtrlType )
{
  cerr << "Reached CtrlHandler: " << fdwCtrlType << endl;

  switch( fdwCtrlType )
  {
    // Handle the CTRL-C signal.
    case CTRL_C_EVENT:
       sigint_handler((int)fdwCtrlType);
      return( TRUE );

    default:
      return FALSE;
  }
}
#endif

prompttype getprompt()
{
    return prompt;
}

void setprompt(prompttype p, string arg)
{
    prompt = p;

    if (p == COMMAND)
    {
        console_setecho(true);
    }
    else
    {
        pw_buf_pos = 0;
        if (arg.size())
        {
            OUTSTREAM << arg << flush;
        }
        else
        {
            OUTSTREAM << prompts[p] << flush;
        }
        console_setecho(false);
    }
}

// readline callback - exit if EOF, add to history unless password
static void store_line(char* l)
{
    procesingline = true;
    if (!l)
    {
#ifndef _WIN32 // to prevent exit with Supr key
        doExit = true;
        rl_set_prompt("(CTRL+D) Exiting ...\n");
#ifndef NDEBUG
        if (comms->serverinitiatedfromshell)
        {
            OUTSTREAM << " Forwarding exit command to the server, since this cmd shell (most likely) initiated it" << endl;
            comms->executeCommand("exit", readconfirmationloop);
        }
#endif
#endif
        return;
    }

    if (*l && ( prompt == COMMAND ))
    {
        add_history(l);
    }

    line = l;
}

#ifdef _WIN32

bool validoptionforreadline(const string& string)
{// TODO: this has not been tested in 100% cases (perhaps it is too diligent or too strict)
    int c,i,ix,n,j;
    for (i=0, ix=string.length(); i < ix; i++)
    {
        c = (unsigned char) string[i];

        //if (c>0xC0) return false;
        //if (c==0x09 || c==0x0a || c==0x0d || (0x20 <= c && c <= 0x7e) ) n = 0; // is_printable_ascii
        if (0x00 <= c && c <= 0x7f) n=0; // 0bbbbbbb
        else if ((c & 0xE0) == 0xC0) n=1; // 110bbbbb
        else if ( c==0xed && i<(ix-1) && ((unsigned char)string[i+1] & 0xa0)==0xa0) return false; //U+d800 to U+dfff
        else if ((c & 0xF0) == 0xE0) {return false; n=2;} // 1110bbbb
        else if ((c & 0xF8) == 0xF0) {return false; n=3;} // 11110bbb
        //else if (($c & 0xFC) == 0xF8) n=4; // 111110bb //byte 5, unnecessary in 4 byte UTF-8
        //else if (($c & 0xFE) == 0xFC) n=5; // 1111110b //byte 6, unnecessary in 4 byte UTF-8
        else return false;
        for (j=0; j<n && i<ix; j++) { // n bytes matching 10bbbbbb follow ?
            if ((++i == ix) || (( (unsigned char)string[i] & 0xC0) != 0x80))
                return false;
        }
    }
    return true;
}

bool validwcharforeadline(const wchar_t thewchar)
{
    wstring input;
    input+=thewchar;
    string output;
    localwtostring(&input,&output);
    return validoptionforreadline(output);
}

wstring escapereadlinebreakers(const wchar_t *what)
{
    wstring output;
    for( u_int i = 0; i < wcslen( what ) ; i++ )
    {
        if(validwcharforeadline(what[ i ] ))
        {
            output.reserve( output.size() + 1 );
            output += what[ i ];
        } else {
            wchar_t code[ 7 ];
            swprintf( code, 7, L"\\u%0.4X", what[ i ] ); //while this does not work (yet) as what, at least it shows something and does not break
            //TODO: ideally we would do the conversion from escaped unicode chars \uXXXX back to wchar_t in the server
            // NOTICE: I was able to execute a command with a literl \x242ee (which correspond to \uD850\uDEEE in UTF16).
            // So it'll be more interesting to output here the complete unicode char and in unescapeutf16escapedseqs revert it.
            //     or keep here the UTF16 escaped secs and revert them correctly in the unescapeutf16escapedseqs
            output.reserve( output.size() + 7 ); // "\u"(2) + 5(uint max digits capacity)
            output += code;
        }
    }
    return output;
}
#endif

void install_rl_handler(const char *theprompt)
{
#ifdef _WIN32
    wstring wswhat;
    stringtolocalw(theprompt,&wswhat);
    const wchar_t *what = wswhat.c_str();


    // escape characters that break readline input (e.g. Chinese ones. e.g \x242ee)
    wstring output = escapereadlinebreakers(what);

    // give readline something it understands
    what = output.c_str();
    size_t buffer_size;
    wcstombs_s(&buffer_size, NULL, 0, what, _TRUNCATE);

    if (buffer_size) //coversion is ok
    {
        // do the actual conversion
        char *buffer = new char[buffer_size];
        wcstombs_s(&buffer_size, buffer, buffer_size,what, _TRUNCATE);
        rl_callback_handler_install(buffer, store_line);
    }
    else
    {
        rl_callback_handler_install("INVALID_PROMPT: ", store_line);
    }

#else
    rl_callback_handler_install(theprompt, store_line);
#endif
}

void changeprompt(const char *newprompt, bool redisplay)
{
    if (*dynamicprompt)
    {
        if (!strcmp(newprompt,dynamicprompt))
            return; //same prompt. do nth
    }
    mutexPrompt.lock();

    strncpy(dynamicprompt, newprompt, sizeof( dynamicprompt ));

    if (redisplay)
    {
        // save line
        int saved_point = rl_point;
        char *saved_line = rl_copy_text(0, rl_end);

        rl_clear_message();

        // enter a new line if not processing sth (otherwise, the newline should already be there)
        if (!procesingline)
        {
            rl_crlf();
        }

        install_rl_handler(*dynamicprompt ? dynamicprompt : prompts[COMMAND]);

        // restore line
        if (saved_line)
        {
            rl_replace_line(saved_line, 0);
            free(saved_line);
            saved_line = NULL;
        }
        rl_point = saved_point;
        rl_redisplay();

        handlerinstalled = true;

        requirepromptinstall = false;
    }
    mutexPrompt.unlock();
}

void escapeEspace(string &orig)
{
    replaceAll(orig," ", "\\ ");
}

void unescapeEspace(string &orig)
{
    replaceAll(orig,"\\ ", " ");
}

char* empty_completion(const char* text, int state)
{
    // we offer 2 different options so that it doesn't complete (no space is inserted)
    if (state == 0)
    {
        return strdup(" ");
    }
    if (state == 1)
    {
        return strdup(text);
    }
    return NULL;
}

char* generic_completion(const char* text, int state, vector<string> validOptions)
{
    static size_t list_index, len;
    static bool foundone;
    string name;
    if (!validOptions.size()) // no matches
    {
        return empty_completion(text,state); //dont fall back to filenames
    }
    if (!state)
    {
        list_index = 0;
        foundone = false;
        len = strlen(text);
    }
    while (list_index < validOptions.size())
    {
        name = validOptions.at(list_index);
        if (!rl_completion_quote_character) {
            escapeEspace(name);
        }

        list_index++;

        if (!( strcmp(text, "")) || (( name.size() >= len ) && ( strlen(text) >= len ) && ( name.find(text) == 0 )))
        {
            if (name.size() && (( name.at(name.size() - 1) == '=' ) || ( name.at(name.size() - 1) == '/' )))
            {
                rl_completion_suppress_append = 1;
            }
            foundone = true;
            return dupstr((char*)name.c_str());
        }
    }

    if (!foundone)
    {
        return empty_completion(text,state); //dont fall back to filenames
    }

    return((char*)NULL );
}

inline bool ends_with(std::string const & value, std::string const & ending)
{
    if (ending.size() > value.size()) return false;
    return std::equal(ending.rbegin(), ending.rend(), value.rbegin());
}

char* local_completion(const char* text, int state)
{
    return((char*)NULL );  //matches will be NULL: readline will use local completion
}

void pushvalidoption(vector<string>  *validOptions, const char *beginopt)
{
#ifdef _WIN32
    if (validoptionforreadline(beginopt))
    {
        validOptions->push_back(beginopt);
    }
    else
    {
        wstring input;
        stringtolocalw(beginopt,&input);
        wstring output = escapereadlinebreakers(input.c_str());

        string soutput;
        localwtostring(&output,&soutput);
        validOptions->push_back(soutput.c_str());
    }
#else
    validOptions->push_back(beginopt);
#endif
}


char* remote_completion(const char* text, int state)
{
    char *saved_line = strdup(getCurrentLine().c_str());

    static vector<string> validOptions;
    if (state == 0)
    {
        validOptions.clear();
        string completioncommand("completionshell ");
        completioncommand+=saved_line;

        OUTSTRING s;
        OUTSTRINGSTREAM oss(s);

        comms->executeCommand(completioncommand, readconfirmationloop, oss);

        string outputcommand;

#ifdef _WIN32
        localwtostring(&oss.str(),&outputcommand);
#else
         outputcommand = oss.str();
#endif

        if (outputcommand == "MEGACMD_USE_LOCAL_COMPLETION")
        {
            free(saved_line);
            return local_completion(text,state); //fallback to local path completion
        }

        char *ptr = (char *)outputcommand.c_str();

        char *beginopt = ptr;
        while (*ptr)
        {
            if (*ptr == 0x1F)
            {
                *ptr = '\0';
                if (strcmp(beginopt," ")) //the server will give a " " for empty_completion (no matches)
                {
                    pushvalidoption(&validOptions,beginopt);
                }

                beginopt=ptr+1;
            }
            ptr++;
        }
        if (*beginopt && strcmp(beginopt," "))
        {
            pushvalidoption(&validOptions,beginopt);
        }
    }

    free(saved_line);
    saved_line = NULL;

    return generic_completion(text, state, validOptions);
}

static char** getCompletionMatches(const char * text, int start, int end)
{
    rl_filename_quoting_desired = 1;

    char **matches;

    matches = (char**)NULL;

    matches = rl_completion_matches((char*)text, remote_completion);

    return( matches );
}

void printHistory()
{
    int length = history_length;
    int offset = 1;
    int rest = length;
    while (rest >= 10)
    {
        offset++;
        rest = rest / 10;
    }

    for (int i = 0; i < length; i++)
    {
        history_set_pos(i);
        OUTSTREAM << setw(offset) << i << "  " << current_history()->line << endl;
    }
}

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>


/**
 * @brief getcharacterreadlineUTF16support
 * while this works, somehow arrows and other readline stuff is disabled using this one.
 * @param stream
 * @return
 */
int getcharacterreadlineUTF16support (FILE *stream)
{
    int result;
    char b[10];
    memset(b,0,10);

    while (1)
    {
        int oldmode = _setmode(fileno(stream), _O_U16TEXT);

        result = read (fileno (stream), &b, 10);
        _setmode(fileno(stream), oldmode);

        if (result == 0)
        {
            return (EOF);
        }

        // convert the UTF16 string to widechar
        size_t wbuffer_size;
        mbstowcs_s(&wbuffer_size, NULL, 0, b, _TRUNCATE);
        wchar_t *wbuffer = new wchar_t[wbuffer_size];
        mbstowcs_s(&wbuffer_size, wbuffer, wbuffer_size, b, _TRUNCATE);

        // convert the UTF16 widechar to UTF8 string
        string receivedutf8;
        utf16ToUtf8(wbuffer, wbuffer_size,&receivedutf8);

        if (strlen(receivedutf8.c_str()) > 1) //multi byte utf8 sequence: place the UTF8 characters into rl buffer one by one
        {
            for (u_int i=0;i< strlen(receivedutf8.c_str());i++)
            {
                rl_line_buffer[rl_end++] = receivedutf8.c_str()[i];
                rl_point=rl_end;
            }
            rl_line_buffer[rl_end] = '\0';

            return 0;
        }

        if (result =! 0)
        {
            return (b[0]);
        }

        /* If zero characters are returned, then the file that we are
     reading from is empty!  Return EOF in that case. */
        if (result == 0)
        {
            return (EOF);
        }
    }
}
#endif

void wait_for_input(int readline_fd)
{
    fd_set fds;

    FD_ZERO(&fds);
    FD_SET(readline_fd, &fds);

    int rc = select(FD_SETSIZE, &fds, NULL, NULL, NULL);
    if (rc < 0)
    {
        if (ERRNO != EINTR)  //syscall
        {
#ifdef _WIN32
         if (ERRNO != WSAENOTSOCK) // it enters here since it is not a socket. Alt: Use WaitForMultipleObjectsEx
#endif
                cerr << "Error at select at wait_for_input errno: " << ERRNO << endl;
            return;
        }
    }
}

bool isserverloggedin()
{
    if (comms->executeCommand(("loggedin")) == MCMD_NOTLOGGEDIN )
    {
        return false;
    }
    return true;
}

vector<string> getlistOfWords(char *ptr, bool ignoreTrailingSpaces = true)
{
    vector<string> words;

    char* wptr;

    // split line into words with quoting and escaping
    for (;; )
    {
        // skip leading blank space
        while (*ptr > 0 && *ptr <= ' ' && (ignoreTrailingSpaces || *(ptr+1)))
        {
            ptr++;
        }

        if (!*ptr)
        {
            break;
        }

        // quoted arg / regular arg
        if (*ptr == '"')
        {
            ptr++;
            wptr = ptr;
            words.push_back(string());

            for (;; )
            {
                if (( *ptr == '"' ) || ( *ptr == '\\' ) || !*ptr)
                {
                    words[words.size() - 1].append(wptr, ptr - wptr);

                    if (!*ptr || ( *ptr++ == '"' ))
                    {
                        break;
                    }

                    wptr = ptr - 1;
                }
                else
                {
                    ptr++;
                }
            }
        }
        else if (*ptr == '\'') // quoted arg / regular arg
        {
            ptr++;
            wptr = ptr;
            words.push_back(string());

            for (;; )
            {
                if (( *ptr == '\'' ) || ( *ptr == '\\' ) || !*ptr)
                {
                    words[words.size() - 1].append(wptr, ptr - wptr);

                    if (!*ptr || ( *ptr++ == '\'' ))
                    {
                        break;
                    }

                    wptr = ptr - 1;
                }
                else
                {
                    ptr++;
                }
            }
        }
        else
        {
            while (*ptr == ' ') ptr++;// only possible if ptr+1 is the end

            wptr = ptr;

            char *prev = ptr;
            //while ((unsigned char)*ptr > ' ')
            while ((*ptr != '\0') && !(*ptr ==' ' && *prev !='\\'))
            {
                if (*ptr == '"')
                {
                    while (*++ptr != '"' && *ptr != '\0')
                    { }
                }
                prev=ptr;
                ptr++;
            }

                words.push_back(string(wptr, ptr - wptr));
        }
    }

    return words;
}


void process_line(char * line)
{
    switch (prompt)
    {
        case AREYOUSURE:
            //this is currently never used
            if (!strcasecmp(line,"yes") || !strcasecmp(line,"y"))
            {
                comms->setResponseConfirmation(true);
                setprompt(COMMAND);
            }
            else if (!strcasecmp(line,"no") || !strcasecmp(line,"n"))
            {
                comms->setResponseConfirmation(false);
                setprompt(COMMAND);
            }
            else
            {
                //Do nth, ask again
                OUTSTREAM << "Please enter: [y]es/[n]o: " << flush;
            }
            break;
        case LOGINPASSWORD:
        {
            if (!strlen(line))
            {
                break;
            }
            if (!confirminglink)
            {
                string logincommand("login -v ");
                logincommand+=loginname;
                logincommand+=" " ;
                logincommand+=line;

                comms->executeCommand(logincommand.c_str(), readconfirmationloop);
            }
            else
            {
                string confirmcommand("confirm ");
                confirmcommand+=linktoconfirm;
                confirmcommand+=" " ;
                confirmcommand+=loginname;
                confirmcommand+=" " ;
                confirmcommand+=line;

                comms->executeCommand(confirmcommand.c_str(), readconfirmationloop);

                confirminglink = false;
            }

            setprompt(COMMAND);
            break;
        }

        case OLDPASSWORD:
        {
            if (!strlen(line))
            {
                break;
            }
            oldpasswd = line;
            OUTSTREAM << endl;
            setprompt(NEWPASSWORD);
            break;
        }

        case NEWPASSWORD:
        {
            if (!strlen(line))
            {
                break;
            }
            newpasswd = line;
            OUTSTREAM << endl;
            setprompt(PASSWORDCONFIRM);
        }
            break;

        case PASSWORDCONFIRM:
        {
            if (!strlen(line))
            {
                break;
            }
            if (line != newpasswd)
            {
                OUTSTREAM << endl << "New passwords differ, please try again" << endl;
            }
            else
            {
                OUTSTREAM << endl;
                string changepasscommand("passwd ");
                changepasscommand+=oldpasswd;
                changepasscommand+=" " ;
                changepasscommand+=newpasswd;

                comms->executeCommand(changepasscommand.c_str(), readconfirmationloop);
            }

            setprompt(COMMAND);
            break;
        }
        case COMMAND:
        {
            vector<string> words = getlistOfWords(line);
            if (words.size())
            {
                if ( words[0] == "exit" || words[0] == "quit")
                {
                    if (words.size() == 1)
                    {
                        doExit = true;
                    }
                    if (words.size() == 1 || words[1]!="--only-shell")
                    {
                        comms->executeCommand(line, readconfirmationloop);
                    }
                    else
                    {
                        doExit = true;
                    }
                }
                else if (words[0] == "history")
                {
                    printHistory();
                }
#ifdef _WIN32
                else if (words[0] == "unicode" && words.size() == 1)
                {
                    rl_getc_function=(rl_getc_function==&getcharacterreadlineUTF16support)?rl_getc:&getcharacterreadlineUTF16support;
                    OUTSTREAM << "Unicode shell input " << ((rl_getc_function==&getcharacterreadlineUTF16support)?"ENABLED":"DISABLED") << endl;
                    return;
                }
#endif
                else if (words[0] == "passwd")
                {

                    if (isserverloggedin())
                    {
                        if (words.size() == 1)
                        {
                            setprompt(OLDPASSWORD);
                        }
                        else
                        {
                            comms->executeCommand(line, readconfirmationloop);
                        }
                    }
                    else
                    {
                        cerr << "Not logged in." << endl;
                    }

                    return;
                }
                else if (words[0] == "login")
                {
                    if (!isserverloggedin())
                    {
                        if (words.size() == 2)
                        {
                            loginname = words[1];
                            setprompt(LOGINPASSWORD);
                        }
                        else
                        {
                            comms->executeCommand(line, readconfirmationloop);
                        }
                    }
                    else
                    {
                        cerr << "Already logged in. Please log out first." << endl;
                    }
                    return;
                }
                else if (words[0] == "confirm")
                {
                    if (words.size() == 3)
                    {
                        linktoconfirm = words[1];
                        loginname = words[2];
                        confirminglink = true;
                        setprompt(LOGINPASSWORD);
                    }
                    else
                    {
                        comms->executeCommand(line, readconfirmationloop);
                    }
                }
                else if ( words[0] == "clear" )
                {
#ifdef _WIN32
                    HANDLE hStdOut;
                    CONSOLE_SCREEN_BUFFER_INFO csbi;
                    DWORD count;

                    hStdOut = GetStdHandle( STD_OUTPUT_HANDLE );
                    if (hStdOut == INVALID_HANDLE_VALUE) return;

                    /* Get the number of cells in the current buffer */
                    if (!GetConsoleScreenBufferInfo( hStdOut, &csbi )) return;
                    /* Fill the entire buffer with spaces */
                    if (!FillConsoleOutputCharacter( hStdOut, (TCHAR) ' ', csbi.dwSize.X *csbi.dwSize.Y, { 0, 0 }, &count ))
                    {
                        return;
                    }
                    /* Fill the entire buffer with the current colors and attributes */
                    if (!FillConsoleOutputAttribute(hStdOut, csbi.wAttributes, csbi.dwSize.X *csbi.dwSize.Y, { 0, 0 }, &count))
                    {
                        return;
                    }
                    /* Move the cursor home */
                    SetConsoleCursorPosition( hStdOut, { 0, 0 } );
#else
                    rl_clear_screen(0,0);
#endif
                    return;
                }
                else if (words[0] == "transfers")
                {
                    string toexec;

                    if (!strstr (line,"path-display-size"))
                    {
                        u_int width = getNumberOfCols(75);
                        int pathSize = int((width-45)/2);

                        toexec+="transfers --path-display-size=";
                        toexec+=SSTR(pathSize);
                        toexec+=" ";
                        if (strlen(line)>10)
                        {
                            toexec+=line+10;
                        }
                    }
                    else
                    {
                        toexec+=line;
                    }

                    comms->executeCommand(toexec.c_str(), readconfirmationloop);
                }
                else
                {
                    // execute user command
                    comms->executeCommand(line, readconfirmationloop);
                }
            }
            else
            {
                cerr << "failed to interprete input line: " << line << endl;
            }
            break;
        }
    }

}

// main loop
void readloop()
{
    time_t lasttimeretrycons = 0;

    char *saved_line = NULL;
    int saved_point = 0;

    rl_save_prompt();

    int readline_fd = -1;

    readline_fd = fileno(rl_instream);

    comms->registerForStateChanges(statechangehandle);

    //give it a while to communicate the state
    sleepMicroSeconds(1);

#if defined(_WIN32) && defined(USE_PORT_COMMS)
    // due to a failure in reconnecting to the socket, if the server was initiated in while registeringForStateChanges
    // in windows we would not be yet connected. we need to manually try to register again.
    if (comms->registerAgainRequired)
    {
        comms->registerForStateChanges(statechangehandle);
    }
    //give it a while to communicate the state
    sleepMicroSeconds(1);
#endif

    for (;; )
    {
        if (prompt == COMMAND)
        {
            mutexPrompt.lock();
            if (requirepromptinstall)
            {
                install_rl_handler(*dynamicprompt ? dynamicprompt : prompts[COMMAND]);

                handlerinstalled = false;

                // display prompt
                if (saved_line)
                {
                    rl_replace_line(saved_line, 0);
                    free(saved_line);
                    saved_line = NULL;
                }

                rl_point = saved_point;
                rl_redisplay();
            }
            mutexPrompt.unlock();
        }



        // command editing loop - exits when a line is submitted
        for (;; )
        {
            if (prompt == COMMAND || prompt == AREYOUSURE)
            {
                procesingline = false;

                wait_for_input(readline_fd);

                time_t tnow = time(NULL);
                if ( (tnow - lasttimeretrycons) > 5)
                {
                    comms->executeCommand("retrycons");
                    lasttimeretrycons = tnow;
                }

                rl_callback_read_char(); //this calls store_line if last char was enter

                rl_resize_terminal(); // to always adjust to new screen sizes

                if (doExit)
                {
                    if (saved_line != NULL)
                        free(saved_line);
                    saved_line = NULL;
                    return;
                }
            }
            else
            {
                console_readpwchar(pw_buf, sizeof pw_buf, &pw_buf_pos, &line);
            }

            if (line)
            {
                break;
            }
        }

        // save line
        saved_point = rl_point;
        saved_line = rl_copy_text(0, rl_end);

        // remove prompt
        rl_save_prompt();
        rl_replace_line("", 0);
        rl_redisplay();

        if (line)
        {
            if (strlen(line))
            {
                mutexPrompt.lock();
                process_line(line);
                requirepromptinstall = true;
                mutexPrompt.unlock();

                if (comms->registerAgainRequired)
                {
                    // register again for state changes
                     comms->registerForStateChanges(statechangehandle);
                     comms->registerAgainRequired = false;
                }

                // sleep, so that in case there was a changeprompt waiting, gets executed before relooping
                // this is not 100% guaranteed to happen
                sleepSeconds(0);
            }
            free(line);
            line = NULL;
        }
        if (doExit)
        {
            if (saved_line != NULL)
                free(saved_line);
            saved_line = NULL;
            return;
        }
    }
}

class NullBuffer : public std::streambuf
{
public:
    int overflow(int c)
    {
        return c;
    }
};

void printCenteredLine(string msj, u_int width, bool encapsulated = true)
{
    if (msj.size()>width)
    {
        width = msj.size();
    }
    if (encapsulated)
        COUT << "|";
    for (u_int i = 0; i < (width-msj.size())/2; i++)
        COUT << " ";
    COUT << msj;
    for (u_int i = 0; i < (width-msj.size())/2 + (width-msj.size())%2 ; i++)
        COUT << " ";
    if (encapsulated)
        COUT << "|";
    COUT << endl;
}

void printWelcomeMsg(u_int width)
{
    if (!width)
    {
        width = getNumberOfCols(75);
    }

    COUT << endl;
    COUT << ".";
    for (u_int i = 0; i < width; i++)
        COUT << "=" ;
    COUT << ".";
    COUT << endl;
    printCenteredLine(" __  __ _____ ____    _                      _ ",width);
    printCenteredLine("|  \\/  | ___|/ ___|  / \\   ___ _ __ ___   __| |",width);
    printCenteredLine("| |\\/| | \\  / |  _  / _ \\ / __| '_ ` _ \\ / _` |",width);
    printCenteredLine("| |  | | /__\\ |_| |/ ___ \\ (__| | | | | | (_| |",width);
    printCenteredLine("|_|  |_|____|\\____/_/   \\_\\___|_| |_| |_|\\__,_|",width);

    COUT << "|";
    for (u_int i = 0; i < width; i++)
        COUT << " " ;
    COUT << "|";
    COUT << endl;
    printCenteredLine("Welcome to MEGAcmd! A Command Line Interactive and Scriptable",width);
    printCenteredLine("Application to interact with your MEGA account",width);
    printCenteredLine("This is a BETA version, it might not be bug-free.",width);
    printCenteredLine("Also, the signature/output of the commands may change in a future.",width);
    printCenteredLine("Please write to support@mega.nz if you find any issue or",width);
    printCenteredLine("have any suggestion concerning its functionalities.",width);
    printCenteredLine("Enter \"help --non-interactive\" to learn how to use MEGAcmd with scripts.",width);
    printCenteredLine("Enter \"help\" for basic info and a list of available commands.",width);

#ifdef _WIN32
    printCenteredLine("Enter \"help --unicode\" for info regarding non-ASCII support.",width);
#endif

    COUT << "`";
    for (u_int i = 0; i < width; i++)
        COUT << "=" ;
    COUT << "´";
    COUT << endl;

}

int quote_detector(char *line, int index)
{
    return (
        index > 0 &&
        line[index - 1] == '\\' &&
        !quote_detector(line, index - 1)
    );
}


bool runningInBackground()
{
#ifndef _WIN32
    pid_t fg = tcgetpgrp(STDIN_FILENO);
    if(fg == -1) {
        // Piped:
        return false;
    }  else if (fg == getpgrp()) {
        // foreground
        return false;
    } else {
        // background
        return true;
    }
#endif
    return false;
}

#ifdef _WIN32
void mycompletefunct(char **c, int num_matches, int max_length)
{
    int rows = 1, cols = 80;

#if defined( RL_ISSTATE ) && defined( RL_STATE_INITIALIZED )

            if (RL_ISSTATE(RL_STATE_INITIALIZED))
            {
                rl_resize_terminal();
                rl_get_screen_size(&rows, &cols);
            }
#endif

    OUTSTREAM << endl;
    int nelements_per_col = (cols-1)/(max_length+1);
    for (int i=1; i <= num_matches; i++) //contrary to what the documentation says, num_matches is not the size of c (but num_matches+1), current text is preappended in c[0]
    {
        OUTSTREAM << setw(max_length+1) << left << c[i];
        if ( (i%nelements_per_col == 0) && (i != num_matches))
        {
            OUTSTREAM << endl;
        }
    }
    OUTSTREAM << endl;
}
#endif

bool readconfirmationloop(const char *question)
{
    bool firstime = true;
    for (;; )
    {
        string response;

        if (firstime)
        {
            response = readline(question);

        }
        else
        {
            response = readline("Please enter [y]es/[n]o:");

        }

        firstime = false;

        if (response == "yes" || response == "y" || response == "YES" || response == "Y")
        {
            rl_callback_handler_remove();
            return true;
        }
        if (response == "no" || response == "n" || response == "NO" || response == "N")
        {
            rl_callback_handler_remove();
            return false;
        }
    }
}


int main(int argc, char* argv[])
{
#ifdef _WIN32
    // Set Environment's default locale
    setlocale(LC_ALL, "");
    rl_completion_display_matches_hook = mycompletefunct;
#endif

#ifdef __MACH__
    initializeMacOSStuff(argc,argv);
#endif

    mutexPrompt.init(false);

    // intialize the comms object
#if defined(_WIN32) && !defined(USE_PORT_COMMS)
    comms = new MegaCmdShellCommunicationsNamedPipes();
#else
    comms = new MegaCmdShellCommunications();
#endif

#if _WIN32
    if( !SetConsoleCtrlHandler( (PHANDLER_ROUTINE) CtrlHandler, TRUE ) )
    {
        cerr << "Control handler set failed" << endl;
     }
#else
    // prevent CTRL+C exit
    signal(SIGINT, sigint_handler);
#endif

    rl_attempted_completion_function = getCompletionMatches;
    rl_completer_quote_characters = "\"'";
    rl_filename_quote_characters  = " ";
    rl_completer_word_break_characters = (char *)" ";

    rl_char_is_quoted_p = &quote_detector;

    if (!runningInBackground())
    {
        rl_initialize(); // initializes readline,
        // so that we can use rl_message or rl_resize_terminal safely before ever
        // prompting anything.
    }

#ifdef _WIN32
    // in windows, rl_resize_terminal fails to resize before first prompt appears, we take the width from elsewhere
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    int columns;
    GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi);
    columns = csbi.srWindow.Right - csbi.srWindow.Left - 2;
    printWelcomeMsg(columns);
#else
    sleepMicroSeconds(200); // this gives a little while so that the console is ready and rl_resize_terminal works fine
    printWelcomeMsg();
#endif

    readloop();


    clear_history();
    rl_callback_handler_remove(); //To avoid having the terminal messed up (requiring a "reset")
    delete comms;

}
