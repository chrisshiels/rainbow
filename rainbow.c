/* 'rainbow.c'. */
/* Chris Shiels.  2019. */


/*
  - Tested:
    - asciiquarium  - ok.
    - cmatrix       - ok.
    - emacs         - ok.
    - less          - ok.
    - man           - ok.
    - mc            - ok.
    - mutt          - ok.
    - nethack       - ok.
    - nyancat       - ok.
    - reset         - ok.
    - robots        - ok.
    - rogue         - ok.
    - screen        - ok.
    - sl            - ok.
    - tabs          - ok.
    - tmux          - ok.
    - top           - ok.
    - vim           - ok.


  - Bugs:
    - Rainbow colour display for bash ^r is not consistent
      and this looks to be because instead of rewriting characters
      bash / readline generates ^[[nP - DCH and relies on the
      terminal to scroll existing output.
    - Rainbow colour display when scrolling with vim ^e and ^y is not consistent
      and this looks to be because instead of rewriting characters
      vim uses scrolling regions and ^[[L to scroll existing output.
    - Rainbow colour display for less ^l is not consistent
      and this looks to be because less simply regenerates all lines of output
      from the bottom of the page.


  - Fix:
    - Fix returnperror() exit codes.
    - Add 8bit support via command line flag - might be necessary for
      Linux console.
    - Have fewer dark colours.
    - Need to add buffer overflow protection for keep.
    - Leave ansisequence after n unrecognised bytes.
    - Leave utf8 after 4 unrecognised bytes.


  - Ideas:
    - Use cursor position report when starting.
    - Invert select loop and parser.
    - Explore curses and scrolling terminals.
*/


#define _XOPEN_SOURCE 500


#define DEFAULT_SHELL "/bin/bash"


#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>


int returnperror(const char *s, int status) {
  perror(s);
  return status;
}


/*
  - See:
    - lolcat.
      - https://github.com/busyloop/lolcat

    - Making Rainbows With Ruby, Waves And A Flux Capacitor.
      - http://nikolay.rocks/2015-10-24-waves-rainbows-and-flux
*/
int rainbow(float freq, float i, int *red, int *green, int *blue) {
  *red   = sin(freq * i + 0) * 127 + 128;
  *green = sin(freq * i + 2 * M_PI / 3) * 127 + 128;
  *blue  = sin(freq * i + 4 * M_PI / 3) * 127 + 128;
  return 0;
}


int ansicolour8bit(FILE *stream, int red, int green, int blue) {
  int red1 = red / 256.0 * 5;
  int green1 = green / 256.0 * 5;
  int blue1 = blue / 256.0 * 5;
  int colour = 16 + 36 * red1 + 6 * green1 + blue1;
  return fprintf(stream, "\x1b[38;5;%dm", colour);
}


int ansicolour24bit(FILE *stream, int red, int green, int blue) {
  return fprintf(stream, "\x1b[38;2;%d;%d;%dm", red, green, blue);
}


int ansicolourreset(FILE *stream) {
  return fputs("\x1b[0m", stream);
}


int pty(int *fdmaster, int *fdslave) {
  if ((*fdmaster = open("/dev/ptmx", O_RDWR)) == -1)
    return returnperror("open()", -1);

  if (grantpt(*fdmaster) == -1)
    return returnperror("grantpt()", -1);

  if (unlockpt(*fdmaster) == -1)
    return returnperror("unlockpt()", -1);

  const char *name;
  if ((name = ptsname(*fdmaster)) == NULL)
    return returnperror("ptsname()", -1);

  if ((*fdslave = open(name, O_RDWR)) == -1)
    return returnperror("open()", -1);

  return 0;
}


int windowsizecopy(int fdfrom, int fdto) {
  struct winsize w;

  if (ioctl(fdfrom, TIOCGWINSZ, &w) == -1)
    return returnperror("ioctl()", -1);
  if (ioctl(fdto, TIOCSWINSZ, &w) == -1)
    return returnperror("ioctl()", -1);

  return 0;
}


static int g_fdstdin;
static int g_fdmaster;
static int g_fdslave;


void signalchildstoppedorterminated() {
  close(g_fdslave);
}


void signalwindowresize() {
  windowsizecopy(g_fdstdin, g_fdmaster);
  signal(SIGWINCH, signalwindowresize);
}


int signals(int fdstin, int fdmaster, int fdslave) {
  g_fdstdin = fdstin;
  g_fdmaster = fdmaster;
  g_fdslave = fdslave;

  if (signal(SIGCHLD, signalchildstoppedorterminated) == SIG_ERR)
    return returnperror("signal()", -1);

  if (signal(SIGWINCH, signalwindowresize) == SIG_ERR)
    return returnperror("signal()", -1);

  return 0;
}


int termiosraw(int fd, struct termios *t) {
  if (tcgetattr(fd, t) == -1)
    return returnperror("tcgetattr()", -1);

  struct termios t2 = *t;
  t2.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  t2.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  t2.c_cflag &= ~(CSIZE | PARENB);
  t2.c_cflag |= CS8;
  t2.c_oflag &= ~(OPOST);
  t2.c_cc[VMIN] = 1;
  t2.c_cc[VTIME] = 0;

  if (tcsetattr(fd, TCSAFLUSH, &t2) == -1)
    return returnperror("tcsetattr()", -1);

  return 0;
}


int termiosreset(int fd, const struct termios *t) {
  if (tcsetattr(fd, TCSAFLUSH, t) == -1)
    return returnperror("tcsetattr()", -1);

  return 0;
}


int parsenandm(const char *s, int *n, int *m) {
  *n = 0;
  *m = 0;

  for (; *s && isdigit(*s); s++) {
    *n *= 10;
    *n += *s - '0';
  }

  if (*s++ != ';')
    return 0;

  for (; *s && isdigit(*s); s++) {
    *m *= 10;
    *m += *s - '0';
  }

  return 0;
}


typedef void *(*parserfunction)(float freq, float spread, float os,
                                int *row, int *column,
                                char *keep, int *keepi,
                                char ch);


void *parseescapesequence(float freq, float spread, float os,
                          int *row, int *column,
                          char *keep, int *keepi,
                          char ch);


void *parseutf8(float freq, float spread, float os,
                int *row, int *column,
                char *keep, int *keepi,
                char ch);


void *parsetext(float freq, float spread, float os,
                int *row, int *column,
                char *keep, int *keepi,
                char ch);


/*
  - See:
    - ANSI escape code.
      - https://en.wikipedia.org/wiki/ANSI_escape_code

    - ANSI Escape sequences - VT100 / VT52.
      - http://ascii-table.com/ansi-escape-sequences-vt-100.php

    - XTerm Control Sequences.
      - https://invisible-island.net/xterm/ctlseqs/ctlseqs.html

    - Linux console escape and control sequences.
      - console_codes(4)

    - Terminal type descriptions source file.
      - https://invisible-island.net/ncurses/terminfo.src.html
*/
void *parseescapesequence(float freq, float spread, float os,
                          int *row, int *column,
                          char *keep, int *keepi,
                          char ch) {
  static int prevrow;
  static int prevcolumn;

  keep[(*keepi)++] = ch;

  const char *xtermenablealternativebuffer = "\x1b[?1049h";
  const char *xtermdisablealternativebuffer = "\x1b[?1049l";
  if (/* xterm:  Enable alternative screen buffer: */
        (*keepi == strlen(xtermenablealternativebuffer)) &&
        (strncmp(keep, xtermenablealternativebuffer, *keepi) == 0)) {
    prevrow = *row;
    prevcolumn = *column;
  }
  else if (/* xterm:  Disable alternative screen buffer: */
             (*keepi == strlen(xtermdisablealternativebuffer)) &&
             (strncmp(keep, xtermdisablealternativebuffer, *keepi) == 0)) {
    *row = prevrow;
    *column = prevcolumn;
  }
  else if (/* ANSI:  RIS - Reset. */
             *keepi == 2 && keep[1] == 'c') {
    *row = 1;
    *column = 1;
  }

  if (/* ANSI:  CSI - Control Sequence Introducer: */
        keep[1] == '[' &&
        (isalpha(keep[*keepi - 1]) || keep[*keepi - 1] == '@')) {
    int n;
    int m;

    parsenandm(keep + 2, &n, &m);

    if (n == 0)
      n = 1;

    if (m == 0)
      m = 1;

    switch (keep[*keepi - 1]) {
    case 'A': /* ANSI:  'CSI n A' - CUU - Cursor Up: */
              *row -= n;
              break;
    case 'B': /* ANSI:  'CSI n B' - CUD - Cursor Down: */
              *row += n;
              break;
    case 'C': /* ANSI:  'CSI n C' - CUF - Cursor Forward: */
              *column += n;
              break;
    case 'D': /* ANSI:  'CSI n D' - CUB - Cursor Back: */
              *column -= n;
              break;
    case 'E': /* ANSI:  'CSI n E' - CNL - Cursor Next Line: */
              *row += n;
              *column = 1;
              break;
    case 'F': /* ANSI:  'CSI n F' - CPL - Cursor Previous Line: */
              *row -= n;
              *column = 1;
              break;
    case 'G': /* ANSI:  'CSI n G' - CHA - Cursor Horizontal Absolute: */
              *column = n;
              break;
    case 'H': /* ANSI:  'CSI n ; m H' - CUP - Cursor Position: */
              *row = n;
              *column = m;
              break;
    case 'f': /* ANSI:  'CSI n ; m f' - HVP - Horizontal Vertical Position: */
              *row = n;
              *column = m;
              break;
    case '@': /* ANSI:  'CSI n @' - ICH - Insert Characters: */
              break;
    }
    keep[*keepi] = '\0';
    fputs(keep, stdout);
    *keepi = 0;
    return parsetext;
  }

  if (/* ANSI:  OSC - Operating System Command: */
      /*  - Used by vte to report state, e.g. */
      /*    ESC ] 777;notify;Command completed;sleep 5\a, then, */
      /*    ESC ] 0;chris@holzer:~/c\a, then, */
      /*    ESC ] 7;file://hostname.domainname/home/chris/c\a */
        (keep[1] == ']' && keep[*keepi - 1] == '\a') ||
      /* ANSI:  OSC - Operating System Command: */
        (keep[1] == ']' &&
         keep[*keepi - 2] == '\x1b' && keep[*keepi - 1] == '\\') ||
      /* ANSI:  DCS - Device Control String: */
        (keep[1] == 'P' &&
         keep[*keepi - 2] == '\x1b' && keep[*keepi - 1] == '\\') ||
      /* Other: */
        (*keepi == 3 && keep[1] == '(') ||
        (*keepi == 3 && keep[1] == ')') ||
        (*keepi == 2 && keep[1] == '=') ||
        (*keepi == 2 && keep[1] == '>') ||
        (*keepi == 2 && keep[1] == '7') ||
        (*keepi == 2 && keep[1] == '8') ||
        (*keepi == 2 && keep[1] == 'H') ||
        (*keepi == 2 && keep[1] == 'M') ||
        (*keepi == 2 && keep[1] == 'c')) {
    keep[*keepi] = '\0';
    fputs(keep, stdout);
    *keepi = 0;
    return parsetext;
  }

  if (/* screen/tmux:  'ESC k title ESC \' - Set title - Emitted by nyancat */
        keep[1] == 'k' ||
        keep[1] == '\\') {
    keep[*keepi] = '\0';
    fputs(keep, stdout);
    *keepi = 0;
    return parsetext;
  }

  return parseescapesequence;
}


void *parseutf8(float freq, float spread, float os,
                int *row, int *column,
                char *keep, int *keepi,
                char ch) {
  int red;
  int green;
  int blue;

  keep[(*keepi)++] = ch;

  if ((*keepi == 2 && (((unsigned char)keep[0] >> 5) == 0b110)) ||
      (*keepi == 3 && (((unsigned char)keep[0] >> 4) == 0b1110)) ||
      (*keepi == 4 && (((unsigned char)keep[0] >> 3) == 0b11110))) {
    *column += 1;
    keep[*keepi] = '\0';
    rainbow(freq, os + *row + *column / spread, &red, &green, &blue);
    ansicolour24bit(stdout, red, green, blue);
    fputs(keep, stdout);
    *keepi = 0;
    return parsetext;
  }

  return parseutf8;
}


void *parsetext(float freq, float spread, float os,
                int *row, int *column,
                char *keep, int *keepi,
                char ch) {
  int red;
  int green;
  int blue;

  if (ch == '\x1b') {
    *keepi = 0;
    keep[(*keepi)++] = ch;
    return parseescapesequence;
  }

  if (ch & 128) {
    *keepi = 0;
    keep[(*keepi)++] = ch;
    return parseutf8;
  }

  if (ch == '\n') {
    *row += 1;
    *column = 1;
  } else if (ch == '\b')
    *column -= 1;
  else if (ch == '\r')
    *column = 1;
  else if (ch == '\t')
    *column += 8 - (*column % 8);
  else
    *column += 1;

  rainbow(freq, os + *row + *column / spread, &red, &green, &blue);
  ansicolour24bit(stdout, red, green, blue);
  fputc(ch, stdout);
  return parsetext;
}


int output(FILE *stdout,
           const char *buf,
           int count,
           float freq,
           float spread,
           float os) {
  static int row = 1;
  static int column = 1;

  static char keep[1024];
  static int keepi;

  static parserfunction parser = parsetext;

  int i;
  for (i = 0; i < count; i++) {
    parser = parser(freq, spread, os, &row, &column, keep, &keepi, buf[i]);
  }

  for (;;) {
    fflush(stdout);
    if (!ferror(stdout))
      break;
    clearerr(stdout);
  }

  return 0;
}


int loop(FILE *stdout, int fdstdin, int fdmaster, int childpid) {
  float freq = 0.1;
  float spread = 3.0;
  float os = random() * 1.0 / RAND_MAX * 255;

  fd_set readfds;
  char buf[1024];
  int nread;

  for (;;) {
    FD_ZERO(&readfds);
    FD_SET(fdstdin, &readfds);
    FD_SET(fdmaster, &readfds);

    if (select(fdmaster + 1, &readfds, NULL, NULL, NULL) == -1) {
      if (errno == EINTR)
        continue;
      else if (errno == EBADF)
        break;
      else
        return returnperror("select()", -1);
    }

    if (FD_ISSET(fdstdin, &readfds)) {
      nread = read(fdstdin, buf, 1024);
      if (nread == -1)
        return returnperror("read()", -1);
      else if (write(fdmaster, buf, nread) != nread)
        return returnperror("write()", -1);
    }

    if (FD_ISSET(fdmaster, &readfds)) {
      nread = read(fdmaster, buf, 1024);
      if (nread == 0 ||
          (nread == -1 && errno == EIO))
        break;
      else if (nread == -1)
        return returnperror("read()", -1);
      else if (output(stdout, buf, nread, freq, spread, os) == -1)
        return returnperror("output()", -1);
    }
  }

  return 0;
}


int parent(int fdmaster, int fdslave, int childpid) {
  if (windowsizecopy(STDIN_FILENO, fdmaster) == -1)
    return -1;

  if (signals(STDIN_FILENO, fdmaster, fdslave) == -1)
    return -1;

  struct termios t;
  if (termiosraw(STDIN_FILENO, &t) == -1)
    return -1;

  if (loop(stdout, STDIN_FILENO, fdmaster, childpid) == -1)
    return -1;

  if (termiosreset(STDIN_FILENO, &t) == -1)
    return -1;

  if (ansicolourreset(stdout) == -1)
    return -1;

  return 0;
}


int child(int fdslave, const char **argv, const char **envp) {
  if (setsid() == -1)
    return returnperror("setsid()", -1);

  if (dup2(fdslave, STDIN_FILENO) == -1 ||
      dup2(fdslave, STDOUT_FILENO) == -1 ||
      dup2(fdslave, STDERR_FILENO) == -1)
    return returnperror("dup2()", -1);

  execve(argv[0], (char * const*)argv, (char * const*)envp);

  return returnperror("execve()", -1);
}


int start(const char **argv, const char **envp) {
  if (access(argv[0], F_OK | X_OK) == -1)
    return returnperror("access()", -1);

  srandom(time(NULL));

  int fdmaster, fdslave;
  if (pty(&fdmaster, &fdslave) == -1)
    return EXIT_FAILURE;

  int pid = fork();
  if (pid == -1)
    return returnperror("fork()", -1);
  else if (pid != 0)
    return parent(fdmaster, fdslave, pid);
  else
    return child(fdslave, argv, envp);

  return EXIT_SUCCESS;
}


char *searchpath(const char *var, const char *name, char *buf, int buflen) {
  const char *value = getenv(var);
  if (!value)
    return NULL;

  char *value1 = strdup(value);
  if (!value1)
    return NULL;

  char *s;
  for (s = strtok(value1, ":"); s != NULL; s = strtok(NULL, ":")) {
    snprintf(buf, buflen, "%s/%s", s, name);
    if (access(buf, F_OK | X_OK) == 0)
      break;
  }

  free(value1);

  if (!s) {
    errno = ENOENT;
    return NULL;
  }
  else
    return buf;
}


int usage(FILE *stream, int status) {
  fputs("Usage:  rainbow [ command [ arg ... ] ]\n", stream);
  return status;
}


int startshell(const char **argv, const char **envp) {
  char *shell = getenv("SHELL");
  if (!shell)
    shell = DEFAULT_SHELL;
  return start((const char *[]){ shell, NULL }, envp);
}


int startpath(const char **argv, const char **envp) {
  return start(argv + 1, envp);
}


int startsearchpath(const char **argv, const char **envp) {
  char buf[FILENAME_MAX];
  if (!searchpath("PATH", argv[1], buf, FILENAME_MAX))
    return returnperror("access()", -1);
  argv[1] = buf;
  return start(argv + 1, envp);
}


int main(int argc, const char **argv, const char **envp) {
  if (argc == 2 && strcmp(argv[1], "--help") == 0)
    return usage(stdout, EXIT_SUCCESS);
  else if (argc == 1)
    return startshell(argv, envp);
  else if (strchr(argv[1], '/'))
    return startpath(argv, envp);
  else
    return startsearchpath(argv, envp);
}
