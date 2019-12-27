/* 'rainbow.c'. */
/* Chris Shiels.  2019. */


/*
  - See:
    - Making Rainbows With Ruby, Waves And A Flux Capacitor.
      - http://nikolay.rocks/2015-10-24-waves-rainbows-and-flux
    - busyloop / lolcat.
      - https://github.com/busyloop/lolcat


  - Tested:
    - cmatrix       - ok.
    - emacs         - ok.
    - less          - ok.
    - man           - ok.
    - mc            - ok.
    - mutt          - ok.
    - reset         - ok.
    - robots        - ok.
    - screen        - ok.
    - tmux          - ok.
    - top           - ok.
    - vim           - ok.


  - Fix:
    - Add 8bit support via command line flag - might be necessary for
      Linux console.
    - Have less dark colours.
    - Move rainbow inputs around.
    - Rethink os, i in terms of os, row, column and CSI CUP.
    - Rethink parsing all CSI sequences and maintaining row, column instead
      of os and i.
    - Need to add buffer overflow protection for keep.
    - Crashes when running Flask.
    - rogue had problems.
    - readline editing.
    - Leave ansisequence after 20 unrecognised bytes.
    - Leave utf8 after 4 unrecognised bytes.
    - Move ansisequence conditions to separate parse functions.
    - ^l results in odd colour change for first line only.
    - vim insert mode colour transitions are too coarse.


  - Done:
    - Control-c behaviour ok.
    - Control-z behaviour ok.
    - Control-\ behaviour ok.
    - Resizing windows works with vi ok.
    - Environment correctly set in child shell ok.
    - Exiting child bash exits cleanly ok.


  - To do:
    - Default to $SHELL and then default to /bin/bash.
    - Need much more robust ansi escape sequence parser.
    - Allow specifying command line to run.
    - Allow specifying to filter.


  - Hmmmm:
    - We're faster than lolcat - guessing they're flushing per character but
      we're using C stream buffering on stdout in order to minimise writes.
*/


#define _XOPEN_SOURCE 500


#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>


int returnperror(const char *s, int status) {
  perror(s);
  return status;
}


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
  return fprintf(stream, "\x1b[0m");
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


void signalchildstoppedorterminated() {
  close(g_fdmaster);
}


void signalwindowresize() {
  windowsizecopy(g_fdstdin, g_fdmaster);
  signal(SIGWINCH, signalwindowresize);
}


int signals(int fdstin, int fdmaster) {
  g_fdstdin = fdstin;
  g_fdmaster = fdmaster;

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


/*
  - Notes on ANSI escape sequences:

    - See:
      console_codes(4)

    - See:
      https://en.wikipedia.org/wiki/ANSI_escape_code
    
    - Sequences:

      - ST  - String Terminator:              ESC \

      - CSI - Control Sequence Introducer:    ESC [ data

      - OSC - Operating System Command:       ESC ] data <ST>

        - Used by vte to report state, e.g.
          ESC ] 777;notify;Command completed;sleep 5\a, then,
          ESC ] 0;chris@holzer:~/c\a, then,
          ESC ] 7;file://holzer.home.mecachis.net/home/chris/c\a

      - DCS - Device Control String:          ESC P data <ST>

        - https://invisible-island.net/xterm/ctlseqs/ctlseqs.html
          'Set Termcap /Terminfo Data (xterm, experimental)'.

  - Notes on vt100 escape sequences:

    - See:
      http://ascii-table.com/ansi-escape-sequences-vt-100.php

    - Used by less.

    - Sequences:

      - Move / scroll window down one line:   ESC M

      - Set United States G0 character set:   ESC ( B
*/
int output(FILE *stdout,
           const char *buf,
           int count,
           float freq,
           float spread,
           float *os,
           int *i) {
  int red;
  int green;
  int blue;

  static enum {
    ansisequence,
    utf8,
    text
  } state = text;
  static char keep[1024];
  static int keepi;

  int j;
  for (j = 0; j < count; j++) {
    if (state == ansisequence) {
      keep[keepi++] = buf[j];
      /* ANSI:  'CSI n ; m H' - CUP - Cursor Position: */
      if (keep[1] == '[' && keep[keepi - 1] == 'H') {
        keep[keepi] = '\0';
        int row, column;
        if (sscanf(keep, "\x1b[%d;%dH", &row, &column) == 2) {
          *i = column;
          *os += 1;
        }
        fprintf(stdout, keep);
        keepi = 0;
        state = text;
        continue;
      }

      if (/* ANSI:  CSI - Control Sequence Introducer: */
            (keep[1] == '[' && isalpha(keep[keepi - 1])) ||
          /* ANSI:  OSC - Operating System Command: */
            (keep[1] == ']' && keep[keepi - 1] == '\a') ||
          /* ANSI:  OSC - Operating System Command: */
            (keep[1] == ']' &&
             keep[keepi - 2] == '\x1b' && keep[keepi - 1] == '\\') ||
          /* ANSI:  DCS - Device Control String: */
            (keep[1] == 'P' &&
             keep[keepi - 2] == '\x1b' && keep[keepi - 1] == '\\') ||
          /* VT100: */
            (keepi == 3 && keep[1] == '(') ||
            (keepi == 3 && keep[1] == ')') ||
            (keepi == 2 && keep[1] == '=') ||
            (keepi == 2 && keep[1] == '>') ||
            (keepi == 2 && keep[1] == 'M') ||
            (keepi == 2 && keep[1] == 'c')) {
        keep[keepi] = '\0';
        fprintf(stdout, keep);
        keepi = 0;
        state = text;
        continue;
      }
    }
    else if (state == utf8) {
      keep[keepi++] = buf[j];
      if ((keepi == 2 && (((unsigned char)keep[0] >> 5) == 0b110)) ||
          (keepi == 3 && (((unsigned char)keep[0] >> 4) == 0b1110)) ||
          (keepi == 4 && (((unsigned char)keep[0] >> 3) == 0b11110))) {
        keep[keepi] = '\0';
        rainbow(freq, *os + *i / spread, &red, &green, &blue);
        ansicolour24bit(stdout, red, green, blue);
        fprintf(stdout, keep);
        keepi = 0;
        state = text;
        continue;
      }
    }
    else if (state == text) {
      if (buf[j] == '\x1b') {
        keepi = 0;
        keep[keepi++] = buf[j];
        state = ansisequence;
        continue;
      }

      if (buf[j] & 128) {
        keepi = 0;
        keep[keepi++] = buf[j];
        state = utf8;
        continue;
      }

      if (buf[j] == '\n') {
        *os += 1;
        *i = 0;
      } else if (buf[j] == '\b')
        *i -= 1;
      else if (buf[j] == '\r')
        *i = 0;
      else
        *i += 1;

      rainbow(freq, *os + *i / spread, &red, &green, &blue);
      ansicolour24bit(stdout, red, green, blue);
      fputc(buf[j], stdout);
    }
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
  int i = 0;

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
      if ((nread = read(fdstdin, buf, 1024)) == -1)
        return returnperror("read()", -1);
      else if (write(fdmaster, buf, nread) != nread)
        return returnperror("write()", -1);
    }

    if (FD_ISSET(fdmaster, &readfds)) {
      if ((nread = read(fdmaster, buf, 1024)) == -1)
        return returnperror("read()", -1);
      else if (output(stdout, buf, nread, freq, spread, &os, &i) == -1)
        return returnperror("output()", -1);
    }
  }

  return 0;
}


int parent(int fdmaster, int childpid) {
  if (windowsizecopy(STDIN_FILENO, fdmaster) == -1)
    return -1;

  if (signals(STDIN_FILENO, fdmaster) == -1)
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


int child(int fdslave, const char **envp) {
  if (setsid() == -1)
    return returnperror("setsid()", -1);

  if (dup2(fdslave, STDIN_FILENO) == -1 ||
      dup2(fdslave, STDOUT_FILENO) == -1 ||
      dup2(fdslave, STDERR_FILENO) == -1)
    return returnperror("dup2()", -1);

  char *newargv[] = { "/bin/bash", NULL };
  execve("/bin/bash", newargv, (char * const*)envp);

  return returnperror("execve()", -1);
}


int start(const char **envp, int fdmaster, int fdslave) {
  int pid = fork();
  if (pid == -1)
    return returnperror("fork()", -1);
  else if (pid != 0)
    return parent(fdmaster, pid);
  else
    return child(fdslave, envp);
}


int main(int argc, const char **argv, const char **envp) {
  srandom(time(NULL));

  int fdmaster, fdslave;
  if (pty(&fdmaster, &fdslave) == -1)
    return EXIT_FAILURE;

  if (start(envp, fdmaster, fdslave) == -1)
    return EXIT_FAILURE;

  return EXIT_SUCCESS;
}
