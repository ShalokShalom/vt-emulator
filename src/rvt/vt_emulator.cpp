/*
*   This program is free software; you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation; either version 2 of the License, or
*   (at your option) any later version.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program; if not, write to the Free Software
*   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*
*   Product name: redemption, a FLOSS RDP proxy
*   Copyright (C) Wallix 2010-2016
*   Author(s): Jonathan Poelen;
*
*   Based on Konsole, an X terminal
*/

#include <string>
#include <algorithm>

#include "utils/sugar/underlying_cast.hpp"

#include "rvt/character.hpp"
#include "rvt/charsets.hpp"
#include "rvt/screen.hpp"
#include "rvt/char_class.hpp"
#include "rvt/vt_emulator.hpp"


#include <fstream>
#include "rvt/utf8_decoder.hpp"

namespace rvt
{

VtEmulator::VtEmulator(int lines, int columns, Screen::LineSaver lineSaver)
: _screen0{lines, columns}
, _screen1{lines, columns}
{
    reset();
    _screen0.setLineSaver(lineSaver);
    _screen1.setLineSaver(std::move(lineSaver));
}

VtEmulator::~VtEmulator() = default;

void VtEmulator::clearEntireScreen()
{
    _currentScreen->clearEntireScreen();
    // bufferedUpdate();
}

void VtEmulator::reset()
{
    // Save the current codec so we can set it later.
    // Ideally we would want to use the profile setting

    resetTokenizer();
    resetModes();
    resetCharset();
    _currentScreen->reset();
}

/* ------------------------------------------------------------------------- */
/*                                                                           */
/*                     Processing the incoming byte stream                   */
/*                                                                           */
/* ------------------------------------------------------------------------- */

/* Incoming Bytes Event pipeline

   This section deals with decoding the incoming character stream.
   Decoding means here, that the stream is first separated into `tokens'
   which are then mapped to a `meaning' provided as operations by the
   `Screen' class or by the emulation class itself.

   The pipeline proceeds as follows:

   - Tokenizing the ESC codes (onReceiveChar)
   - VT100 code page translation of plain characters (applyCharset)
   - Interpretation of ESC codes (processToken)

   The escape codes and their meaning are described in the
   technical reference of this program.
*/

// Tokens ------------------------------------------------------------------ --

/*
   Since the tokens are the central notion if this section, we've put them
   in front. They provide the syntactical elements used to represent the
   terminals operations as byte sequences.

   They are encodes here into a single machine word, so that we can later
   switch over them easily. Depending on the token itself, additional
   argument variables are filled with parameter values.

   The tokens are defined below:

   - CHR        - Printable characters     (32..255 but DEL (=127))
   - CTL        - Control characters       (0..31 but ESC (= 27), DEL)
   - ESC        - Escape codes of the form <ESC><CHR but `[]()+*#'>
   - ESC_DE     - Escape codes of the form <ESC><any of `()+*#%'> C
   - CSI_PN     - Escape codes of the form <ESC>'['     {Pn} ';' {Pn} C
   - CSI_PS     - Escape codes of the form <ESC>'['     {Pn} ';' ...  C
   - CSI_PR     - Escape codes of the form <ESC>'[' '?' {Pn} ';' ...  C
   - CSI_PE     - Escape codes of the form <ESC>'[' '!' {Pn} ';' ...  C
   - CSI_PG     - Escape codes of the form <ESC>'[' '>' {Pn} ';' ...  C
   - DCS        - Escape codes of the form <ESC>'P'              ... '\' (IGNORED)
   - DCS        - Escape codes of the form <ESC>'^'              ... '\' (IGNORED)
   - DCS        - Escape codes of the form <ESC>'_'              ... '\' (IGNORED)
   - VT52       - VT52 escape codes
                  - <ESC><Chr>
                  - <ESC>'Y'{Pc}{Pc}
   - XTE_HA     - Xterm window/terminal attribute commands
                  of the form <ESC>`]' {Pn} `;' {Text} <BEL>
                  (Note that these are handled differently to the other formats)

   The last two forms allow list of arguments. Since the elements of
   the lists are treated individually the same way, they are passed
   as individual tokens to the interpretation. Further, because the
   meaning of the parameters are names (although represented as numbers),
   they are includes within the token ('N').

*/

#define TY_CONSTRUCT(T,A,N) ( \
    (((N+0) & 0xffff) << 16) | \
    (((A+0) & 0xff) << 8) | \
    ( (T+0) & 0xff) )

#define TY_CHR(   )     TY_CONSTRUCT(0,0,0)
#define TY_CTL(A  )     TY_CONSTRUCT(1,A,0)
#define TY_ESC(A  )     TY_CONSTRUCT(2,A,0)
#define TY_ESC_CS(A,B)  TY_CONSTRUCT(3,A,B)
#define TY_ESC_DE(A  )  TY_CONSTRUCT(4,A,0)
#define TY_CSI_PS(A,N)  TY_CONSTRUCT(5,A,N)
#define TY_CSI_PN(A  )  TY_CONSTRUCT(6,A,0)
#define TY_CSI_PR(A,N)  TY_CONSTRUCT(7,A,N)

#define TY_VT52(A)    TY_CONSTRUCT(8,A,0)
#define TY_CSI_PG(A)  TY_CONSTRUCT(9,A,0)
#define TY_CSI_PE(A)  TY_CONSTRUCT(10,A,0)

const int MAX_ARGUMENT = 4096;

// Tokenizer --------------------------------------------------------------- --

/* The tokenizer's state

   The state is represented by the buffer (tokenBuffer, tokenBufferPos),
   and accompanied by decoded arguments kept in (argv,argc).
   Note that they are kept internal in the tokenizer.
*/

void VtEmulator::resetTokenizer()
{
    tokenBufferPos = 0;
    argc = 0;
    argv[0] = 0;
    argv[1] = 0;
}

void VtEmulator::addDigit(int digit)
{
    argv[argc] = std::min(10 * argv[argc] + digit, MAX_ARGUMENT);
}

void VtEmulator::addArgument()
{
    argc = std::min(argc + 1, MAXARGS - 1);
    argv[argc] = 0;
}

void VtEmulator::addToCurrentToken(ucs4_char cc)
{
    tokenBuffer[tokenBufferPos] = cc;
    tokenBufferPos = std::min(tokenBufferPos + 1, MAX_TOKEN_LENGTH - 1);
}

/* Ok, here comes the nasty part of the decoder.

   Instead of keeping an explicit state, we deduce it from the
   token scanned so far. It is then immediately combined with
   the current character to form a scanning decision.

   This is done by the following defines.

   - P is the length of the token scanned so far.
   - L (often P-1) is the position on which contents we base a decision.
   - C is a character or a group of characters (taken from 'charClass').

   - 'cc' is the current character
   - 's' is a pointer to the start of the token buffer
   - 'p' is the current position within the token buffer

   Note that they need to applied in proper order.
*/

#define lec(P,L,C) (p == (P) && s[(L)] == (C))
#define lun(     ) (p ==  1  && cc >= 32 )
#define les(P,L,C) (p == (P) && s[L] < 256 && (charClass[s[(L)]] & (C)) == (C))
#define eec(C)     (p >=  3  && cc == (C))
#define ees(C)     (p >=  3  && cc < 256 && (charClass[cc] & (C)) == (C))
#define eps(C)     (p >=  3  && s[2] != '?' && s[2] != '!' && s[2] != '>' && cc < 256 && (charClass[cc] & (C)) == (C))
#define epp( )     (p >=  3  && s[2] == '?')
#define epe( )     (p >=  3  && s[2] == '!')
#define egt( )     (p >=  3  && s[2] == '>')
#define Xpe        (tokenBufferPos >= 2 && tokenBuffer[1] == ']')
#define Xte        (Xpe      && cc ==  7 )
#define ces(C)     (cc < 256 && (charClass[cc] & (C)) == (C) && !Xte)

#define CNTL(c) ((c)-'@')
const int ESC = 27;
const int DEL = 127;

// process an incoming unicode character
void VtEmulator::receiveChar(ucs4_char cc)
{
    if (cc == DEL)
        return; //VT100: ignore.

    // DCS, PM, APC  (IGNORED) XTerm
    if (tokenBufferPos == 2 && (tokenBuffer[1] == 'P' || tokenBuffer[1] =='^' || tokenBuffer[1] == '_')) {
        if (cc == '\\') {
            resetTokenizer();
        }
        return ;
    }

    if (ces(CTL))
    {
        // DEC HACK ALERT! Control Characters are allowed *within* esc sequences in VT100
        // This means, they do neither a resetTokenizer() nor a pushToToken(). Some of them, do
        // of course. Guess this originates from a weakly layered handling of the X-on
        // X-off protocol, which comes really below this level.
        if (cc == CNTL('X') || cc == CNTL('Z') || cc == ESC)
            resetTokenizer(); //VT100: CAN or SUB
        if (cc != ESC)
        {
            processToken(TY_CTL(cc+'@' ),0,0);
            return;
        }
    }
    // advance the state
    addToCurrentToken(cc);

    ucs4_char * s = tokenBuffer;
    const int  p = tokenBufferPos;

    if (getMode(Mode::Ansi))
    {
        if (lec(1,0,ESC)) { return; }
        if (lec(1,0,ESC+128)) { s[0] = ESC; receiveChar('['); return; }
        if (les(2,1,GRP)) { return; }
        if (Xte         ) { processWindowAttributeRequest(); resetTokenizer(); return; }
        if (Xpe         ) { return; }
        if (lec(2,1,'P')) { return; }
        if (lec(2,1,'^')) { return; }
        if (lec(2,1,'_')) { return; }
        if (lec(3,2,'?')) { return; }
        if (lec(3,2,'>')) { return; }
        if (lec(3,2,'!')) { return; }
        if (lun(       )) { processToken( TY_CHR(), applyCharset(cc), 0);   resetTokenizer(); return; }
        if (lec(2,0,ESC)) { processToken( TY_ESC(s[1]), 0, 0);              resetTokenizer(); return; }
        if (les(3,1,SCS)) { processToken( TY_ESC_CS(s[1],s[2]), 0, 0);      resetTokenizer(); return; }
        if (lec(3,1,'#')) { processToken( TY_ESC_DE(s[2]), 0, 0);           resetTokenizer(); return; }
        if (eps(    CPN)) { processToken( TY_CSI_PN(cc), argv[0], argv[1]); resetTokenizer(); return; }

        // resize = \e[8;<row>;<col>t
        if (eps(CPS))
        {
            processToken( TY_CSI_PS(cc, argv[0]), argv[1], argv[2]);
            resetTokenizer();
            return;
        }

        if (epe(   )) { processToken( TY_CSI_PE(cc), 0, 0); resetTokenizer(); return; }
        if (ees(DIG)) { addDigit(cc-'0'); return; }
        if (eec(';')) { addArgument();    return; }
        for (int i = 0; i <= argc; i++)
        {
            if (epp())
                processToken(TY_CSI_PR(cc,argv[i]), 0, 0);
            else if (egt())
                processToken(TY_CSI_PG(cc), 0, 0); // spec. case for ESC]>0c or ESC]>c
            else if (cc == 'm' && argc - i >= 4 && (argv[i] == 38 || argv[i] == 48) && argv[i+1] == 2)
            {
                // ESC[ ... 48;2;<red>;<green>;<blue> ... m -or- ESC[ ... 38;2;<red>;<green>;<blue> ... m
                i += 2;
                processToken(TY_CSI_PS(cc, argv[i-2]), static_cast<int>(ColorSpace::RGB), (argv[i] << 16) | (argv[i+1] << 8) | argv[i+2]);
                i += 2;
            }
            else if (cc == 'm' && argc - i >= 2 && (argv[i] == 38 || argv[i] == 48) && argv[i+1] == 5)
            {
                // ESC[ ... 48;5;<index> ... m -or- ESC[ ... 38;5;<index> ... m
                i += 2;
                processToken(TY_CSI_PS(cc, argv[i-2]), static_cast<int>(ColorSpace::Index256), argv[i]);
            }
            else
                processToken(TY_CSI_PS(cc,argv[i]), 0, 0);
        }
        resetTokenizer();
    }
    else
    {
        // VT52 Mode
        if (lec(1,0,ESC))
            return;
        if (les(1,0,CHR))
        {
            processToken( TY_CHR(), s[0], 0);
            resetTokenizer();
            return;
        }
        if (lec(2,1,'Y'))
            return;
        if (lec(3,1,'Y'))
            return;
        if (p < 4)
        {
            processToken(TY_VT52(s[1] ), 0, 0);
            resetTokenizer();
            return;
        }
        processToken(TY_VT52(s[1]), s[2], s[3]);
        resetTokenizer();
        return;
    }
}

void VtEmulator::processWindowAttributeRequest()
{
    // Describes the window or terminal session attribute to change
    // See "Operating System Controls" section on http://rtfm.etla.org/xterm/ctlseq.html
    int attribute = 0;
    int i;
    for (i = 2; i < tokenBufferPos     &&
                tokenBuffer[i] >= '0'  &&
                tokenBuffer[i] <= '9'; i++)
    {
        attribute = 10 * attribute + (tokenBuffer[i]-'0');
    }

    if (tokenBuffer[i] != ';')
    {
        reportDecodingError();
        return;
    }

    if (attribute == 0 || attribute == 2) {
        windowTitleLen = std::copy(tokenBuffer+i+1, tokenBuffer+tokenBufferPos-1, windowTitle) - windowTitle;
        windowTitle[windowTitleLen] = 0;
    }
}

// Interpreting Codes ---------------------------------------------------------

constexpr inline CharsetId char_to_charset_id(char c)
{
    return('0' == c) ? CharsetId::VT100Graphics
        : ('A' == c) ? CharsetId::IBMPC
        : ('B' == c) ? CharsetId::Latin1
        : ('U' == c) ? CharsetId::IBMPC
        : ('K' == c) ? CharsetId::UserDefined
        : CharsetId::Undefined;
}

/*
   Now that the incoming character stream is properly tokenized,
   meaning is assigned to them. These are either operations of
   the current _screen, or of the emulation class itself.

   The token to be interpreted comes in as a machine word
   possibly accompanied by two parameters.

   Likewise, the operations assigned to, come with up to two
   arguments. One could consider to make up a proper table
   from the function below.

   The technical reference manual provides more information
   about this mapping.
*/

void VtEmulator::processToken(int token, int32_t p, int q)
{
  switch (token)
  {
    case TY_CHR(         ) : _currentScreen->displayCharacter     (static_cast<ucs4_char>(p)); break; //UTF16

    //             127 DEL    : ignored on input

    case TY_CTL('@'      ) : /* NUL: ignored                      */ break;
    case TY_CTL('A'      ) : /* SOH: ignored                      */ break;
    case TY_CTL('B'      ) : /* STX: ignored                      */ break;
    case TY_CTL('C'      ) : /* ETX: ignored                      */ break;
    case TY_CTL('D'      ) : /* EOT: ignored                      */ break;
    case TY_CTL('F'      ) : /* ACK: ignored                      */ break;
    case TY_CTL('G'      ) : /* TODO emit stateSet(NOTIFYBELL);*/         break; //VT100
    case TY_CTL('H'      ) : _currentScreen->backspace            (          ); break; //VT100
    case TY_CTL('I'      ) : _currentScreen->tab                  (          ); break; //VT100
    case TY_CTL('J'      ) : _currentScreen->newLine              (          ); break; //VT100
    case TY_CTL('K'      ) : _currentScreen->newLine              (          ); break; //VT100
    case TY_CTL('L'      ) : _currentScreen->newLine              (          ); break; //VT100
    case TY_CTL('M'      ) : _currentScreen->toStartOfLine        (          ); break; //VT100

    case TY_CTL('N'      ) :      useCharset           (         1); break; //VT100
    case TY_CTL('O'      ) :      useCharset           (         0); break; //VT100

    case TY_CTL('P'      ) : /* DLE: ignored                      */ break;
    case TY_CTL('Q'      ) : /* DC1: XON continue                 */ break; //VT100
    case TY_CTL('R'      ) : /* DC2: ignored                      */ break;
    case TY_CTL('S'      ) : /* DC3: XOFF halt                    */ break; //VT100
    case TY_CTL('T'      ) : /* DC4: ignored                      */ break;
    case TY_CTL('U'      ) : /* NAK: ignored                      */ break;
    case TY_CTL('V'      ) : /* SYN: ignored                      */ break;
    case TY_CTL('W'      ) : /* ETB: ignored                      */ break;
    case TY_CTL('X'      ) : _currentScreen->displayCharacter     (    0x2592); break; //VT100
    case TY_CTL('Y'      ) : /* EM : ignored                      */ break;
    case TY_CTL('Z'      ) : _currentScreen->displayCharacter     (    0x2592); break; //VT100
    case TY_CTL('['      ) : /* ESC: cannot be seen here.         */ break;
    case TY_CTL('\\'     ) : /* FS : ignored                      */ break;
    case TY_CTL(']'      ) : /* GS : ignored                      */ break;
    case TY_CTL('^'      ) : /* RS : ignored                      */ break;
    case TY_CTL('_'      ) : /* US : ignored                      */ break;

    case TY_ESC('D'      ) : _currentScreen->index                (          ); break; //VT100
    case TY_ESC('E'      ) : _currentScreen->nextLine             (          ); break; //VT100
    case TY_ESC('H'      ) : _currentScreen->changeTabStop        (true      ); break; //VT100
    case TY_ESC('M'      ) : _currentScreen->reverseIndex         (          ); break; //VT100
    case TY_ESC('c'      ) :      reset                (          ); break;

    case TY_ESC('l'      ) : /* IGNORED: Memory Lock.  Locks memory above the cursor.       */ break; //HP
    case TY_ESC('m'      ) : /* IGNORED: Memory Unlock.                                     */ break; //HP
    case TY_ESC('|'      ) : /* TODO Invoke the G3 Character Set as GL (LS3R).              */ break; //XTerm
    case TY_ESC('}'      ) : /* TODO Invoke the G2 Character Set as GL (LS2R).              */ break; //XTerm
    case TY_ESC('~'      ) : /* TODO Invoke the G1 Character Set as GL (LS1R).              */ break; //XTerm
    case TY_ESC('F'      ) : /* IGNORED: Cursor to lower left corner of screen              */ break; //XTerm
    case TY_ESC('N'      ) : /* TODO set G2.  This affects next character only.             */ break; //XTerm
    case TY_ESC('O'      ) : /* TODO set G3.  This affects next character only.             */ break; //XTerm

    //case TY_ESC('P'      ) : /* IGNORED: Device Control String (DCS).                     */ break; //XTerm
    //case TY_ESC('^'      ) : /* IGNORED: Privacy Message (PM).                            */ break; //XTerm
    //case TY_ESC('_'      ) : /* IGNORED: Application Program Command (APC).               */ break; //XTerm
    //case TY_ESC('\\'     ) : /* IGNORED: String Terminator.                               */ break; //XTerm

    case TY_ESC('n'      ) :      useCharset           (         2); break;
    case TY_ESC('o'      ) :      useCharset           (         3); break;
    case TY_ESC('7'      ) :      saveCursor           (          ); break;
    case TY_ESC('8'      ) :      restoreCursor        (          ); break;
    case TY_ESC('6'      ) : /* TODO    Back Index (DECBI)        */ break; //VT420
    case TY_ESC('9'      ) : /* TODO Forward Index (DECFI)        */ break; //VT420

    case TY_ESC('='      ) : /* Enter alternate keypad mode */ break;
    case TY_ESC('>'      ) : /* Exit  alternate keypad mode */ break;
    case TY_ESC('<'      ) :          setMode      (Mode::Ansi     ); break; //VT100

    case TY_ESC_CS('(', '0') :      setCharset           (0, char_to_charset_id('0')); break; //VT100
    case TY_ESC_CS('(', 'A') :      setCharset           (0, char_to_charset_id('A')); break; //VT100
    case TY_ESC_CS('(', 'B') :      setCharset           (0, char_to_charset_id('B')); break; //VT100
    case TY_ESC_CS('(', 'U') :      setCharset           (0, char_to_charset_id('U')); break; //Linux
    case TY_ESC_CS('(', 'K') :      setCharset           (0, char_to_charset_id('K')); break; //Linux

    case TY_ESC_CS(')', '0') :      setCharset           (1, char_to_charset_id('0')); break; //VT100
    case TY_ESC_CS(')', 'A') :      setCharset           (1, char_to_charset_id('A')); break; //VT100
    case TY_ESC_CS(')', 'B') :      setCharset           (1, char_to_charset_id('B')); break; //VT100
    case TY_ESC_CS(')', 'U') :      setCharset           (1, char_to_charset_id('U')); break; //Linux
    case TY_ESC_CS(')', 'K') :      setCharset           (1, char_to_charset_id('K')); break; //Linux

    case TY_ESC_CS('*', '0') :      setCharset           (2, char_to_charset_id('0')); break; //VT100
    case TY_ESC_CS('*', 'A') :      setCharset           (2, char_to_charset_id('A')); break; //VT100
    case TY_ESC_CS('*', 'B') :      setCharset           (2, char_to_charset_id('B')); break; //VT100
    case TY_ESC_CS('*', 'U') :      setCharset           (2, char_to_charset_id('U')); break; //Linux
    case TY_ESC_CS('*', 'K') :      setCharset           (2, char_to_charset_id('K')); break; //Linux

    case TY_ESC_CS('+', '0') :      setCharset           (3, char_to_charset_id('0')); break; //VT100
    case TY_ESC_CS('+', 'A') :      setCharset           (3, char_to_charset_id('A')); break; //VT100
    case TY_ESC_CS('+', 'B') :      setCharset           (3, char_to_charset_id('B')); break; //VT100
    case TY_ESC_CS('+', 'U') :      setCharset           (3, char_to_charset_id('U')); break; //Linux
    case TY_ESC_CS('+', 'K') :      setCharset           (3, char_to_charset_id('K')); break; //Linux

    // case TY_ESC_CS('-', '0') :      setCharset           (3, char_to_charset_id('0')); break; //VT300
    // case TY_ESC_CS('-', 'A') :      setCharset           (3, char_to_charset_id('A')); break; //VT300
    // case TY_ESC_CS('-', 'B') :      setCharset           (3, char_to_charset_id('B')); break; //VT300
    // case TY_ESC_CS('-', 'U') :      setCharset           (3, char_to_charset_id('U')); break; //VT300
    // case TY_ESC_CS('-', 'K') :      setCharset           (3, char_to_charset_id('K')); break; //VT300
    //
    // case TY_ESC_CS('.', '0') :      setCharset           (3, char_to_charset_id('0')); break; //VT300
    // case TY_ESC_CS('.', 'A') :      setCharset           (3, char_to_charset_id('A')); break; //VT300
    // case TY_ESC_CS('.', 'B') :      setCharset           (3, char_to_charset_id('B')); break; //VT300
    // case TY_ESC_CS('.', 'U') :      setCharset           (3, char_to_charset_id('U')); break; //VT300
    // case TY_ESC_CS('.', 'K') :      setCharset           (3, char_to_charset_id('K')); break; //VT300
    //
    // case TY_ESC_CS('/', '0') :      setCharset           (3, char_to_charset_id('0')); break; //VT300
    // case TY_ESC_CS('/', 'A') :      setCharset           (3, char_to_charset_id('A')); break; //VT300
    // case TY_ESC_CS('/', 'B') :      setCharset           (3, char_to_charset_id('B')); break; //VT300
    // case TY_ESC_CS('/', 'U') :      setCharset           (3, char_to_charset_id('U')); break; //VT300
    // case TY_ESC_CS('/', 'K') :      setCharset           (3, char_to_charset_id('K')); break; //VT300

    case TY_ESC_CS('%', 'G') :      /* TODO setCodec             (Utf8Codec   );*/ break; //LINUX
    case TY_ESC_CS('%', '@') :      /* TODO setCodec             (LocaleCodec );*/ break; //LINUX

    case TY_ESC_DE('3'     ) : /* Double height line, top half    */
                               _currentScreen->setLineProperty( LineProperty::DoubleWidth , true );
                               _currentScreen->setLineProperty( LineProperty::DoubleHeight , true );
                                   break;
    case TY_ESC_DE('4'     ) : /* Double height line, bottom half */
                               _currentScreen->setLineProperty( LineProperty::DoubleWidth , true );
                               _currentScreen->setLineProperty( LineProperty::DoubleHeight , true );
                                   break;
    case TY_ESC_DE('5'     ) : /* Single width, single height line*/
                               _currentScreen->setLineProperty( LineProperty::DoubleWidth , false);
                               _currentScreen->setLineProperty( LineProperty::DoubleHeight , false);
                               break;
    case TY_ESC_DE('6'     ) : /* Double width, single height line*/
                               _currentScreen->setLineProperty( LineProperty::DoubleWidth , true);
                               _currentScreen->setLineProperty( LineProperty::DoubleHeight , false);
                               break;
    case TY_ESC_DE('8'     ) : _currentScreen->helpAlign            (          ); break;

// resize = \e[8;<row>;<col>t
    case TY_CSI_PS('t',   8) : setScreenSize( p /*lines */, q /* columns */ ); break;

// change tab text color : \e[28;<color>t  color: 0-16,777,215
    case TY_CSI_PS('t',   28) : /* emit changeTabTextColorRequest      ( p        );*/          break;

    case TY_CSI_PS('K',   0) : _currentScreen->clearToEndOfLine     (          ); break;
    case TY_CSI_PS('K',   1) : _currentScreen->clearToBeginOfLine   (          ); break;
    case TY_CSI_PS('K',   2) : _currentScreen->clearEntireLine      (          ); break;
    case TY_CSI_PS('J',   0) : _currentScreen->clearToEndOfScreen   (          ); break;
    case TY_CSI_PS('J',   1) : _currentScreen->clearToBeginOfScreen (          ); break;
    case TY_CSI_PS('J',   2) : _currentScreen->clearEntireScreen    (          ); break;
    case TY_CSI_PS('J',   3) : /* clearHistory();*/                               break;
    case TY_CSI_PS('g',   0) : _currentScreen->changeTabStop        (false     ); break; //VT100
    case TY_CSI_PS('g',   3) : _currentScreen->clearTabStops        (          ); break; //VT100
    case TY_CSI_PS('h',   4) : _currentScreen->   setMode(ScreenMode::Insert   ); break;
    case TY_CSI_PS('h',  20) :                    setMode(ScreenMode::NewLine  ); break;
    case TY_CSI_PS('i',   0) : /* IGNORED: attached printer          */           break; //VT100
    case TY_CSI_PS('l',   4) : _currentScreen-> resetMode(ScreenMode::Insert  );  break;
    case TY_CSI_PS('l',  20) :                  resetMode(ScreenMode::NewLine  ); break;
    case TY_CSI_PS('n',   0) : /* IGNORED: DSR – Device Status Report */          break; //VT100
    case TY_CSI_PS('n',   3) : /* IGNORED: DSR – Device Status Report */          break; //VT100
    case TY_CSI_PS('n',   5) : /* IGNORED: DSR – Device Status Report */          break; //VT100
    case TY_CSI_PS('n',   6) : /* IGNORED: DSR – Device Status Report */          break; //VT100
    case TY_CSI_PS('s',   0) :      saveCursor           (          ); break;
    case TY_CSI_PS('u',   0) :      restoreCursor        (          ); break;

    case TY_CSI_PS('m',   0) : _currentScreen->setDefaultRendition  (          ); break;
    case TY_CSI_PS('m',   1) : _currentScreen->setRendition          (Rendition::Bold     ); break; //VT100
    case TY_CSI_PS('m',   2) : _currentScreen->setRendition          (Rendition::Dim      ); break; //VT100
    case TY_CSI_PS('m',   3) : _currentScreen->setRendition          (Rendition::Italic   ); break; //VT100
    case TY_CSI_PS('m',   4) : _currentScreen->setRendition          (Rendition::Underline); break; //VT100
    case TY_CSI_PS('m',   5) : _currentScreen->setRendition          (Rendition::Blink    ); break; //VT100
    case TY_CSI_PS('m',   7) : _currentScreen->setRendition          (Rendition::Reverse  ); break;
    case TY_CSI_PS('m',   8) : /* IGNORED: _currentScreen->setRendition          (Rendition::Hidden   );*/ break;
    case TY_CSI_PS('m',  10) : /* IGNORED: mapping related          */ break; //LINUX
    case TY_CSI_PS('m',  11) : /* IGNORED: mapping related          */ break; //LINUX
    case TY_CSI_PS('m',  12) : /* IGNORED: mapping related          */ break; //LINUX
    case TY_CSI_PS('m',  21) : _currentScreen->resetRendition     (Rendition::Bold     ); break;
    case TY_CSI_PS('m',  22) : _currentScreen->resetRendition     (Rendition::Dim      ); break;
    case TY_CSI_PS('m',  23) : _currentScreen->resetRendition     (Rendition::Italic   ); break; //VT100
    case TY_CSI_PS('m',  24) : _currentScreen->resetRendition     (Rendition::Underline); break;
    case TY_CSI_PS('m',  25) : _currentScreen->resetRendition     (Rendition::Blink    ); break;
    case TY_CSI_PS('m',  27) : _currentScreen->resetRendition     (Rendition::Reverse  ); break;
    case TY_CSI_PS('m',  28) : /* IGNORED: _currentScreen->resetRendition     (Rendition::Hidden   ); */ break;

    case TY_CSI_PS('m',   30) : _currentScreen->setForeColor         (ColorSpace::System,  0); break;
    case TY_CSI_PS('m',   31) : _currentScreen->setForeColor         (ColorSpace::System,  1); break;
    case TY_CSI_PS('m',   32) : _currentScreen->setForeColor         (ColorSpace::System,  2); break;
    case TY_CSI_PS('m',   33) : _currentScreen->setForeColor         (ColorSpace::System,  3); break;
    case TY_CSI_PS('m',   34) : _currentScreen->setForeColor         (ColorSpace::System,  4); break;
    case TY_CSI_PS('m',   35) : _currentScreen->setForeColor         (ColorSpace::System,  5); break;
    case TY_CSI_PS('m',   36) : _currentScreen->setForeColor         (ColorSpace::System,  6); break;
    case TY_CSI_PS('m',   37) : _currentScreen->setForeColor         (ColorSpace::System,  7); break;

    case TY_CSI_PS('m',   38) : _currentScreen->setForeColor         (ColorSpace(p),       q); break;

    case TY_CSI_PS('m',   39) : _currentScreen->setForeColor         (ColorSpace::Default,  0); break;

    case TY_CSI_PS('m',   40) : _currentScreen->setBackColor         (ColorSpace::System,  0); break;
    case TY_CSI_PS('m',   41) : _currentScreen->setBackColor         (ColorSpace::System,  1); break;
    case TY_CSI_PS('m',   42) : _currentScreen->setBackColor         (ColorSpace::System,  2); break;
    case TY_CSI_PS('m',   43) : _currentScreen->setBackColor         (ColorSpace::System,  3); break;
    case TY_CSI_PS('m',   44) : _currentScreen->setBackColor         (ColorSpace::System,  4); break;
    case TY_CSI_PS('m',   45) : _currentScreen->setBackColor         (ColorSpace::System,  5); break;
    case TY_CSI_PS('m',   46) : _currentScreen->setBackColor         (ColorSpace::System,  6); break;
    case TY_CSI_PS('m',   47) : _currentScreen->setBackColor         (ColorSpace::System,  7); break;

    case TY_CSI_PS('m',   48) : _currentScreen->setBackColor         (ColorSpace(p),       q); break;

    case TY_CSI_PS('m',   49) : _currentScreen->setBackColor         (ColorSpace::Default,  1); break;

    case TY_CSI_PS('m',   90) : _currentScreen->setForeColor         (ColorSpace::System,  8); break;
    case TY_CSI_PS('m',   91) : _currentScreen->setForeColor         (ColorSpace::System,  9); break;
    case TY_CSI_PS('m',   92) : _currentScreen->setForeColor         (ColorSpace::System, 10); break;
    case TY_CSI_PS('m',   93) : _currentScreen->setForeColor         (ColorSpace::System, 11); break;
    case TY_CSI_PS('m',   94) : _currentScreen->setForeColor         (ColorSpace::System, 12); break;
    case TY_CSI_PS('m',   95) : _currentScreen->setForeColor         (ColorSpace::System, 13); break;
    case TY_CSI_PS('m',   96) : _currentScreen->setForeColor         (ColorSpace::System, 14); break;
    case TY_CSI_PS('m',   97) : _currentScreen->setForeColor         (ColorSpace::System, 15); break;

    case TY_CSI_PS('m',  100) : _currentScreen->setBackColor         (ColorSpace::System,  8); break;
    case TY_CSI_PS('m',  101) : _currentScreen->setBackColor         (ColorSpace::System,  9); break;
    case TY_CSI_PS('m',  102) : _currentScreen->setBackColor         (ColorSpace::System, 10); break;
    case TY_CSI_PS('m',  103) : _currentScreen->setBackColor         (ColorSpace::System, 11); break;
    case TY_CSI_PS('m',  104) : _currentScreen->setBackColor         (ColorSpace::System, 12); break;
    case TY_CSI_PS('m',  105) : _currentScreen->setBackColor         (ColorSpace::System, 13); break;
    case TY_CSI_PS('m',  106) : _currentScreen->setBackColor         (ColorSpace::System, 14); break;
    case TY_CSI_PS('m',  107) : _currentScreen->setBackColor         (ColorSpace::System, 15); break;

    case TY_CSI_PS('q',   0) : /* IGNORED: LEDs off                 */ break; //VT100
    case TY_CSI_PS('q',   1) : /* IGNORED: LED1 on                  */ break; //VT100
    case TY_CSI_PS('q',   2) : /* IGNORED: LED2 on                  */ break; //VT100
    case TY_CSI_PS('q',   3) : /* IGNORED: LED3 on                  */ break; //VT100
    case TY_CSI_PS('q',   4) : /* IGNORED: LED4 on                  */ break; //VT100

    case TY_CSI_PN('@'      ) : _currentScreen->insertChars          (p        ); break;
    case TY_CSI_PN('A'      ) : _currentScreen->cursorUp             (p        ); break; //VT100
    case TY_CSI_PN('B'      ) : _currentScreen->cursorDown           (p        ); break; //VT100
    case TY_CSI_PN('C'      ) : _currentScreen->cursorRight          (p        ); break; //VT100
    case TY_CSI_PN('D'      ) : _currentScreen->cursorLeft           (p        ); break; //VT100
    case TY_CSI_PN('E'      ) : /* Not implemented: cursor next p lines */        break; //VT100
    case TY_CSI_PN('F'      ) : /* Not implemented: cursor preceding p lines */   break; //VT100
    case TY_CSI_PN('G'      ) : _currentScreen->setCursorX           (p        ); break; //LINUX
    case TY_CSI_PN('H'      ) : _currentScreen->setCursorYX          (p,      q); break; //VT100
    case TY_CSI_PN('I'      ) : _currentScreen->tab                  (p        ); break;
    case TY_CSI_PN('L'      ) : _currentScreen->insertLines          (p        ); break;
    case TY_CSI_PN('M'      ) : _currentScreen->deleteLines          (p        ); break;
    case TY_CSI_PN('P'      ) : _currentScreen->deleteChars          (p        ); break;
    case TY_CSI_PN('S'      ) : _currentScreen->scrollUp             (p        ); break;
    case TY_CSI_PN('T'      ) : _currentScreen->scrollDown           (p        ); break;
    case TY_CSI_PN('X'      ) : _currentScreen->eraseChars           (p        ); break;
    case TY_CSI_PN('Z'      ) : _currentScreen->backtab              (p        ); break;
    case TY_CSI_PN('d'      ) : _currentScreen->setCursorY           (p        ); break; //LINUX
    case TY_CSI_PN('f'      ) : _currentScreen->setCursorYX          (p,      q); break; //VT100
    case TY_CSI_PN('r'      ) : setMargins                           (p,      q); break; //VT100
    case TY_CSI_PN('y'      ) : /* IGNORED: Confidence test            */         break; //VT100

    case TY_CSI_PR('h',   1) : /* Enter  cursor key mode */ break; //VT100
    case TY_CSI_PR('l',   1) : /* Exit   cursor key mode */ break; //VT100
    case TY_CSI_PR('s',   1) : /* Save   cursor key mode */ break; //FIXME
    case TY_CSI_PR('r',   1) : /* Retore cursor key mode */ break; //FIXME

    case TY_CSI_PR('l',   2) :        resetMode      (Mode::Ansi     ); break; //VT100

    case TY_CSI_PR('h',   3) :          setMode      (Mode::Columns132); break; //VT100
    case TY_CSI_PR('l',   3) :        resetMode      (Mode::Columns132); break; //VT100

    case TY_CSI_PR('h',   4) : /* Enter Scrolling Mode (DECSCLM) */ break; //VT100
    case TY_CSI_PR('l',   4) : /* Ecit  Scrolling Mode (DECSCLM) */ break; //VT100

    case TY_CSI_PR('h',   5) : _currentScreen->    setMode      (ScreenMode::Screen   ); break; //VT100
    case TY_CSI_PR('l',   5) : _currentScreen->  resetMode      (ScreenMode::Screen   ); break; //VT100

    case TY_CSI_PR('h',   6) : _currentScreen->    setMode      (ScreenMode::Origin   ); break; //VT100
    case TY_CSI_PR('l',   6) : _currentScreen->  resetMode      (ScreenMode::Origin   ); break; //VT100
    case TY_CSI_PR('s',   6) : _currentScreen->   saveMode      (ScreenMode::Origin   ); break; //FIXME
    case TY_CSI_PR('r',   6) : _currentScreen->restoreMode      (ScreenMode::Origin   ); break; //FIXME

    case TY_CSI_PR('h',   7) : _currentScreen->    setMode      (ScreenMode::Wrap     ); break; //VT100
    case TY_CSI_PR('l',   7) : _currentScreen->  resetMode      (ScreenMode::Wrap     ); break; //VT100
    case TY_CSI_PR('s',   7) : _currentScreen->   saveMode      (ScreenMode::Wrap     ); break; //FIXME
    case TY_CSI_PR('r',   7) : _currentScreen->restoreMode      (ScreenMode::Wrap     ); break; //FIXME

    case TY_CSI_PR('h',   8) : /* IGNORED: autorepeat on            */ break; //VT100
    case TY_CSI_PR('l',   8) : /* IGNORED: autorepeat off           */ break; //VT100
    case TY_CSI_PR('s',   8) : /* IGNORED: autorepeat on            */ break; //VT100
    case TY_CSI_PR('r',   8) : /* IGNORED: autorepeat off           */ break; //VT100

    case TY_CSI_PR('h',   9) : /* IGNORED: interlace                */ break; //VT100
    case TY_CSI_PR('l',   9) : /* IGNORED: interlace                */ break; //VT100
    case TY_CSI_PR('s',   9) : /* IGNORED: interlace                */ break; //VT100
    case TY_CSI_PR('r',   9) : /* IGNORED: interlace                */ break; //VT100

    case TY_CSI_PR('h',  12) : /* IGNORED: Cursor blink             */ break; //att610
    case TY_CSI_PR('l',  12) : /* IGNORED: Cursor blink             */ break; //att610
    case TY_CSI_PR('s',  12) : /* IGNORED: Cursor blink             */ break; //att610
    case TY_CSI_PR('r',  12) : /* IGNORED: Cursor blink             */ break; //att610

    case TY_CSI_PR('h',  25) :          setMode      (ScreenMode::Cursor   ); break; //VT100
    case TY_CSI_PR('l',  25) :        resetMode      (ScreenMode::Cursor   ); break; //VT100
    case TY_CSI_PR('s',  25) :         saveMode      (ScreenMode::Cursor   ); break; //VT100
    case TY_CSI_PR('r',  25) :      restoreMode      (ScreenMode::Cursor   ); break; //VT100

    case TY_CSI_PR('h',  40) :         setMode(Mode::AllowColumns132 ); break; // XTERM
    case TY_CSI_PR('l',  40) :       resetMode(Mode::AllowColumns132 ); break; // XTERM

    case TY_CSI_PR('h',  41) : /* IGNORED: obsolete more(1) fix     */ break; //XTERM
    case TY_CSI_PR('l',  41) : /* IGNORED: obsolete more(1) fix     */ break; //XTERM
    case TY_CSI_PR('s',  41) : /* IGNORED: obsolete more(1) fix     */ break; //XTERM
    case TY_CSI_PR('r',  41) : /* IGNORED: obsolete more(1) fix     */ break; //XTERM

    case TY_CSI_PR('h',  47) :          setMode      (Mode::AppScreen); break; //VT100
    case TY_CSI_PR('l',  47) :        resetMode      (Mode::AppScreen); break; //VT100
    case TY_CSI_PR('s',  47) :         saveMode      (Mode::AppScreen); break; //XTERM
    case TY_CSI_PR('r',  47) :      restoreMode      (Mode::AppScreen); break; //XTERM

    case TY_CSI_PR('h',  67) : /* IGNORED: DECBKM                   */ break; //XTERM
    case TY_CSI_PR('l',  67) : /* IGNORED: DECBKM                   */ break; //XTERM
    case TY_CSI_PR('s',  67) : /* IGNORED: DECBKM                   */ break; //XTERM
    case TY_CSI_PR('r',  67) : /* IGNORED: DECBKM                   */ break; //XTERM

    // XTerm defines the following modes:
    // SET_VT200_MOUSE             1000
    // SET_VT200_HIGHLIGHT_MOUSE   1001
    // SET_BTN_EVENT_MOUSE         1002
    // SET_ANY_EVENT_MOUSE         1003

    case TY_CSI_PR('h', 1000) : /*         setMode      (Mode::Mouse1000); */ break; //XTERM
    case TY_CSI_PR('l', 1000) : /*       resetMode      (Mode::Mouse1000); */ break; //XTERM
    case TY_CSI_PR('s', 1000) : /*        saveMode      (Mode::Mouse1000); */ break; //XTERM
    case TY_CSI_PR('r', 1000) : /*     restoreMode      (Mode::Mouse1000); */ break; //XTERM

    case TY_CSI_PR('h', 1001) : /* IGNORED: hilite mouse tracking    */ break; //XTERM
    case TY_CSI_PR('l', 1001) : /*       resetMode      (Mode::Mouse1001); */ break; //XTERM
    case TY_CSI_PR('s', 1001) : /* IGNORED: hilite mouse tracking    */ break; //XTERM
    case TY_CSI_PR('r', 1001) : /* IGNORED: hilite mouse tracking    */ break; //XTERM

    case TY_CSI_PR('h', 1002) : /*         setMode      (Mode::Mouse1002); */ break; //XTERM
    case TY_CSI_PR('l', 1002) : /*       resetMode      (Mode::Mouse1002); */ break; //XTERM
    case TY_CSI_PR('s', 1002) : /*        saveMode      (Mode::Mouse1002); */ break; //XTERM
    case TY_CSI_PR('r', 1002) : /*     restoreMode      (Mode::Mouse1002); */ break; //XTERM

    case TY_CSI_PR('h', 1003) : /*         setMode      (Mode::Mouse1003); */ break; //XTERM
    case TY_CSI_PR('l', 1003) : /*       resetMode      (Mode::Mouse1003); */ break; //XTERM
    case TY_CSI_PR('s', 1003) : /*        saveMode      (Mode::Mouse1003); */ break; //XTERM
    case TY_CSI_PR('r', 1003) : /*     restoreMode      (Mode::Mouse1003); */ break; //XTERM

    case TY_CSI_PR('h',  1004) : /* _reportFocusEvents = true; */ break;
    case TY_CSI_PR('l',  1004) : /* _reportFocusEvents = false; */ break;

    case TY_CSI_PR('h', 1005) : /*         setMode      (Mode::Mouse1005); */ break; //XTERM
    case TY_CSI_PR('l', 1005) : /*       resetMode      (Mode::Mouse1005); */ break; //XTERM
    case TY_CSI_PR('s', 1005) : /*        saveMode      (Mode::Mouse1005); */ break; //XTERM
    case TY_CSI_PR('r', 1005) : /*     restoreMode      (Mode::Mouse1005); */ break; //XTERM

    case TY_CSI_PR('h', 1006) : /*         setMode      (Mode::Mouse1006); */ break; //XTERM
    case TY_CSI_PR('l', 1006) : /*       resetMode      (Mode::Mouse1006); */ break; //XTERM
    case TY_CSI_PR('s', 1006) : /*        saveMode      (Mode::Mouse1006); */ break; //XTERM
    case TY_CSI_PR('r', 1006) : /*     restoreMode      (Mode::Mouse1006); */ break; //XTERM

    case TY_CSI_PR('h', 1015) : /*         setMode      (Mode::Mouse1015); */ break; //URXVT
    case TY_CSI_PR('l', 1015) : /*       resetMode      (Mode::Mouse1015); */ break; //URXVT
    case TY_CSI_PR('s', 1015) : /*        saveMode      (Mode::Mouse1015); */ break; //URXVT
    case TY_CSI_PR('r', 1015) : /*     restoreMode      (Mode::Mouse1015); */ break; //URXVT

    case TY_CSI_PR('h', 1034) : /* IGNORED: 8bitinput activation     */ break; //XTERM

    case TY_CSI_PR('h', 1047) :          setMode      (Mode::AppScreen); break; //XTERM
    case TY_CSI_PR('l', 1047) :        resetMode      (Mode::AppScreen); break; //XTERM
    case TY_CSI_PR('s', 1047) :         saveMode      (Mode::AppScreen); break; //XTERM
    case TY_CSI_PR('r', 1047) :      restoreMode      (Mode::AppScreen); break; //XTERM

    //FIXME: Unitoken: save translations
    case TY_CSI_PR('h', 1048) :      saveCursor           (          ); break; //XTERM
    case TY_CSI_PR('l', 1048) :      restoreCursor        (          ); break; //XTERM
    case TY_CSI_PR('s', 1048) :      saveCursor           (          ); break; //XTERM
    case TY_CSI_PR('r', 1048) :      restoreCursor        (          ); break; //XTERM

    //FIXME: every once new sequences like this pop up in xterm.
    //       Here's a guess of what they could mean.
    case TY_CSI_PR('h', 1049) : saveCursor(); _screen1.clearEntireScreen(); setMode(Mode::AppScreen); break; //XTERM
    case TY_CSI_PR('l', 1049) : resetMode(Mode::AppScreen); restoreCursor(); break; //XTERM

    case TY_CSI_PR('h', 2004) : /*         setMode      (Mode::BracketedPaste); */ break; //XTERM
    case TY_CSI_PR('l', 2004) : /*       resetMode      (Mode::BracketedPaste); */ break; //XTERM
    case TY_CSI_PR('s', 2004) : /*        saveMode      (Mode::BracketedPaste); */ break; //XTERM
    case TY_CSI_PR('r', 2004) : /*     restoreMode      (Mode::BracketedPaste); */ break; //XTERM

    //FIXME: weird DEC reset sequence
    case TY_CSI_PE('p'      ) : /* IGNORED: reset         (        ) */ break;

    //FIXME: when changing between vt52 and ansi mode evtl do some resetting.
    case TY_VT52('A'      ) : _currentScreen->cursorUp             (         1); break; //VT52
    case TY_VT52('B'      ) : _currentScreen->cursorDown           (         1); break; //VT52
    case TY_VT52('C'      ) : _currentScreen->cursorRight          (         1); break; //VT52
    case TY_VT52('D'      ) : _currentScreen->cursorLeft           (         1); break; //VT52

    // FIXME The special graphics characters in the VT100 are different from those in the VT52.
    case TY_VT52('F'      ) :      setAndUseCharset     (0, char_to_charset_id('0')); break; //VT52
    case TY_VT52('G'      ) :      setAndUseCharset     (0, char_to_charset_id('B')); break; //VT52

    case TY_VT52('H'      ) : _currentScreen->setCursorYX          (1,1       ); break; //VT52
    case TY_VT52('I'      ) : _currentScreen->reverseIndex         (          ); break; //VT52
    case TY_VT52('J'      ) : _currentScreen->clearToEndOfScreen   (          ); break; //VT52
    case TY_VT52('K'      ) : _currentScreen->clearToEndOfLine     (          ); break; //VT52
    case TY_VT52('Y'      ) : _currentScreen->setCursorYX          (p-31,q-31 ); break; //VT52
    case TY_VT52('<'      ) :          setMode      (Mode::Ansi     ); break; //VT52
    case TY_VT52('='      ) : /* Enter alternate keypad mode */ break; //VT52
    case TY_VT52('>'      ) : /* Exit  alternate keypad mode */ break; //VT52

    case TY_CSI_PG('c'    ) : /* IGNORED: Send Device Attributes                        */break; //VT100
    case TY_CSI_PG('t'    ) : /* IGNORED: Set one or more features of the title modes.  */break; //XTerm
    case TY_CSI_PG('p'    ) : /* IGNORED: Set resource value pointerMode.               */break; //XTerm

    default:
        reportDecodingError();
        break;
  };
}

void VtEmulator::clearScreenAndSetColumns(int columnCount)
{
    setScreenSize(_currentScreen->getLines(), columnCount);
    clearEntireScreen();
    setDefaultMargins();
    _currentScreen->setCursorYX(0,0);
}

void VtEmulator::setWindowTitle(ucs4_carray_view title) noexcept
{
    this->windowTitleLen = std::min(title.size(), utils::size(this->windowTitle)-1);
    std::copy(title.begin(), title.begin() + this->windowTitleLen, this->windowTitle);
    this->windowTitle[this->windowTitleLen] = 0;
}

/* ------------------------------------------------------------------------- */
/*                                                                           */
/*                                VT100 Charsets                             */
/*                                                                           */
/* ------------------------------------------------------------------------- */

// Character Set Conversion ------------------------------------------------ --

/*
   The processing contains a VT100 specific code translation layer.
   It's still in use and mainly responsible for the line drawing graphics.

   These and some other glyphs are assigned to codes (0x5f-0xfe)
   normally occupied by the latin letters. Since this codes also
   appear within control sequences, the extra code conversion
   does not permute with the tokenizer and is placed behind it
   in the pipeline. It only applies to tokens, which represent
   plain characters.

   This conversion it eventually continued in TerminalDisplay.C, since
   it might involve VT100 enhanced fonts, which have these
   particular glyphs allocated in (0x00-0x1f) in their code page.
*/

// Apply current character map.

#define CHARSET _charsets[_currentScreen == &_screen1]

ucs4_char VtEmulator::applyCharset(ucs4_char c) const
{
    auto const charset_index = underlying_cast(CHARSET.charset_id);
    if (charset_index < underlying_cast(CharsetId::MAX_) && c < charset_map_size) {
        return charset_maps[charset_index][c];
    }
    return c;
}

/*
   "Charset" related part of the emulation state.
   This configures the VT100 charset filter.

   While most operation work on the current _screen,
   the following two are different.
*/

void VtEmulator::resetCharset()
{
    _charsets[0].charset.fill(CharsetId::Latin1);
    _charsets[0].charset_id = CharsetId::Latin1;
    _charsets[0].sa_charset_id = CharsetId::Latin1;

    _charsets[1].charset.fill(CharsetId::Latin1);
    _charsets[1].charset_id = CharsetId::Latin1;
    _charsets[1].sa_charset_id = CharsetId::Latin1;
}

void VtEmulator::setCharset(int n, CharsetId cs) // on both screens.
{
    _charsets[0].charset[n & 3] = cs;
    _charsets[1].charset[n & 3] = cs;
    this->useCharset(n);
}

void VtEmulator::setAndUseCharset(int n, CharsetId cs)
{
    CHARSET.charset[n & 3] = cs;
    useCharset(n);
}

void VtEmulator::useCharset(int n)
{
    CHARSET.charset_id = CHARSET.charset[n & 3];
}

void VtEmulator::setDefaultMargins()
{
    _currentScreen->setDefaultMargins();
}

void VtEmulator::setScreen(int n)
{
    _currentScreen = (n & 1) ? &_screen1 : &_screen0;
}

void VtEmulator::setScreenSize(int lines, int columns)
{
    if (lines < 1 || columns < 1) {
        return;
    }

    _screen0.resizeImage(lines, columns);
    _screen1.resizeImage(lines, columns);
}

void VtEmulator::setMargins(int t, int b)
{
    _currentScreen->setMargins(t, b);
}

void VtEmulator::saveCursor()
{
    CHARSET.sa_charset_id = CHARSET.charset_id;
    // we are not clear about these
    //sa_charset = charsets[cScreen->_charset];
    //sa_charset_num = cScreen->_charset;
    _currentScreen->saveCursor();
}

void VtEmulator::restoreCursor()
{
    CHARSET.charset_id = CHARSET.sa_charset_id;
    _currentScreen->restoreCursor();
}

/* ------------------------------------------------------------------------- */
/*                                                                           */
/*                                Mode Operations                            */
/*                                                                           */
/* ------------------------------------------------------------------------- */

/*
   Some of the emulations state is either added to the state of the screens.

   This causes some scoping problems, since different emulations choose to
   located the mode either to the current _screen or to both.

   For strange reasons, the extend of the rendition attributes ranges over
   all screens and not over the actual _screen.

   We decided on the precise precise extend, somehow.
*/

// "Mode" related part of the state. These are all booleans.

void VtEmulator::resetModes()
{
    // Mode::AllowColumns132 is not reset here
    // to match Xterm's behavior (see Xterm's VTReset() function)

    resetMode(Mode::Columns132); saveMode(Mode::Columns132);
    resetMode(Mode::AppScreen);  saveMode(Mode::AppScreen);
    resetMode(ScreenMode::NewLine);
    setMode(Mode::Ansi);
}

void VtEmulator::setMode(Mode m)
{
    _currentModes.set(m);
    switch (m) {
    case Mode::Columns132:
        if (getMode(Mode::AllowColumns132))
            clearScreenAndSetColumns(132);
        else
            _currentModes.reset(m);
        break;

    case Mode::AppScreen :
        setScreen(1);
        break;

    case Mode::AllowColumns132:
    case Mode::Ansi:
        break;
    }
}

void VtEmulator::resetMode(Mode m)
{
    _currentModes.reset(m);
    switch (m) {
    case Mode::Columns132:
        if (getMode(Mode::AllowColumns132))
            clearScreenAndSetColumns(80);
        break;

    case Mode::AppScreen :
        setScreen(0);
        break;

    case Mode::AllowColumns132:
    case Mode::Ansi:
        break;
    }
}

void VtEmulator::saveMode(Mode m)
{
    _savedModes.copy_of(m, _currentModes);
}

void VtEmulator::restoreMode(Mode m)
{
    _currentModes.copy_of(m, _savedModes);
}

bool VtEmulator::getMode(Mode m)
{
    return _currentModes.has(m);
}

void VtEmulator::setMode(ScreenMode m)
{
    _screen0.setMode(m);
    _screen1.setMode(m);
}

void VtEmulator::resetMode(ScreenMode m)
{
    _screen0.resetMode(m);
    _screen1.resetMode(m);
}

void VtEmulator::saveMode(ScreenMode m)
{
    _screen0.saveMode(m);
    _screen1.saveMode(m);
}

void VtEmulator::restoreMode(ScreenMode m)
{
    _screen0.resetMode(m);
    _screen1.resetMode(m);
}

bool VtEmulator::getMode(ScreenMode m)
{
    return _currentScreen->getMode(m);
}

// return contents of the scan buffer
static std::string hexdump2(ucs4_char const * s, int len)
{
    int i;
    char dump[128];
    std::string returnDump = "Undecodable sequence: ";

    for (i = 0; i < len; i++) {
        if (s[i] == '\\')
            std::snprintf(dump, sizeof(dump), "%s", "\\\\");
        else if ((s[i]) > 32 && s[i] < 127)
            std::snprintf(dump, sizeof(dump), "%c", s[i]);
        else
            std::snprintf(dump, sizeof(dump), "\\x%04x(hex)", s[i]);
        returnDump.append(dump);
    }
    return returnDump;
}

void VtEmulator::reportDecodingError()
{
    if (!_logFunction || tokenBufferPos == 0 || (tokenBufferPos == 1 && (tokenBuffer[0] & 0xff) >= 32)) {
        return;
    }

    _logFunction(hexdump2(tokenBuffer, tokenBufferPos).c_str());
}

}
