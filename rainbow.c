/*
  - Test with:
    - top    - okish.
    - vim    - okish.
    - emacs  - okish.
    - less   - okish.
    - man    - okish.
    - robots - okish.
    - rogue  - okish.
    - reset  - okish.


  - Done:
    - Control-c behaviour ok.
    - Control-z behaviour ok.
    - Control-\ behaviour ok.
    - Resizing windows works with vi ok.
    - Environment correctly set in child shell ok.
    - Exiting child bash exits cleanly ok.


  - To do:
    - Need much more robust ansi escape sequence parser.
    - Allow specifying command line to run.
    - Allow specifying to filter.


  - Hmmmm:
    - We're faster than lolcat - guessing they're flushing per character but
      we're using C stream buffering on stdout in order to minimise writes.
    - Trying to use nonblocking was a mistake:
      - Setting nonblocking on stdin resulted in it also being set on stdout.
      - And we're using C stream buffering but occasional underlying write()s
        would fail with errno == EAGAIN.
      - But C stream wasn't retrying.
      - End result was losing output.
*/


#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 500


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
#include <sys/wait.h>
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

  if (ioctl(fdfrom, TIOCGWINSZ, (char *)&w) == -1)
    return returnperror("ioctl()", -1);
  if (ioctl(fdto, TIOCSWINSZ, (char *)&w) == -1)
    return returnperror("ioctl()", -1);

  return 0;
}


void signalchildstoppedorterminated() {}


static int g_fdstdin;
static int g_fdmaster;


void signalwindowresize() {
  windowsizecopy(g_fdstdin, g_fdmaster);
  signal(SIGWINCH, signalwindowresize);
}


int signals(int fdstin, int fdmaster) {
  if (signal(SIGCHLD, signalchildstoppedorterminated) == SIG_ERR)
    return returnperror("signal()", -1);

  g_fdstdin = fdstin;
  g_fdmaster = fdmaster;
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

  enum {
    text,
    ansisequence,
    utf8
  } state = text;

  static char keep[1024];
  static int keepi;

  for (int j = 0; j < count; j++) {
    if (state == ansisequence) {
      keep[keepi++] = buf[j];
      if ((keep[1] == '[' && isalpha(buf[j])) ||
          (keep[1] == ']' && buf[j] == '\\') ||
          (keep[1] == ']' && buf[j] == '\a') ||
          (keep[1] == 'P' && buf[j] == '\\') ||
          (keepi == 3 && keep[1] == '(') ||
          (keepi == 3 && keep[1] == ')') ||
          (keepi == 2 && keep[1] == '=') ||
          (keepi == 2 && keep[1] == '>') ||
          (keepi == 2 && keep[1] == 'M') ||
          (keepi == 2 && keep[1] == 'c')) {
        keep[keepi] = '\0';
        fprintf(stdout, keep);
        state = text;
        continue;
      }
    }
    else if (state == utf8) {
      keep[keepi++] = buf[j];
      if ((keepi == 2 && (keep[1] & 0b11000000)) ||
          (keepi == 3 && (keep[2] & 0b11100000)) ||
          (keepi == 4 && (keep[3] & 0b11110000))) {
        keep[keepi] = '\0';
        fprintf(stdout, keep);
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
      }

      rainbow(freq, *os + *i / spread, &red, &green, &blue);
      ansicolour24bit(stdout, red, green, blue);
      //ansicolour8bit(stdout, red, green, blue);
      fprintf(stdout, "%c", buf[j]);
      *i += 1;
    }
  }

  fflush(stdout);
  if (ferror(stdout))
    return -1;
  return 0;
}


int loop(FILE *stdout, int fdstdin, int fdmaster, int childpid) {
  float freq = 0.1;
  float spread = 3.0;
  float os = rand() * 1.0 / RAND_MAX * 255;
  //float os = random() * 1.0 / RAND_MAX * 255;
  int i = 0;

  fd_set readfds;
  int ret;
  char buf[8192];
  int nread;

  for (;;) {
    FD_ZERO(&readfds);
    FD_SET(fdstdin, &readfds);
    FD_SET(fdmaster, &readfds);

    if (select(fdmaster + 1, &readfds, NULL, NULL, NULL) == -1) {
      if (errno != EINTR)
        return returnperror("select()", -1);
      else {
        if ((ret = waitpid(childpid, NULL, WNOHANG)) == -1)
          return returnperror("waitpid()", -1);
        else if (ret == childpid)
          break;
        else
          continue;
      }
    }

    if (FD_ISSET(fdstdin, &readfds)) {
      if ((nread = read(fdstdin, buf, 8192)) == -1)
        return returnperror("read()", -1);
      else if (write(fdmaster, buf, nread) != nread)
        return returnperror("write()", -1);
    }

    if (FD_ISSET(fdmaster, &readfds)) {
      if ((nread = read(fdmaster, buf, 8192)) == -1)
        return returnperror("read()", -1);
      else if (output(stdout, buf, nread, freq, spread, &os, &i) == -1)
        return returnperror("output()", -1);
      //else if (write(STDOUT_FILENO, buf, nread) != nread)
      //  return returnperror("write()", -1);
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
  srand(time(NULL));
  //srandom(time(NULL));


#if 0
  float freq = 0.1;
  float spread = 3.0;
  float os = rand() * 1.0 / RAND_MAX * 255;
  //float os = random() * 1.0 / RAND_MAX * 255;

  int red;
  int green;
  int blue;
  const char *message = "No hay mal que por bien no venga\n";
  for (int j = 0; j < 100; j++) {
    os++;
    for (int i = 0; i < strlen(message); i++) {
      rainbow(freq, os + i / spread, &red, &green, &blue);
      ansicolour24bit(stdout, red, green, blue);
      //ansicolour8bit(stdout, red, green, blue);
      fprintf(stdout, "%c", message[i]);
    }
    fflush(stdout);
  }
#endif


  int fdmaster, fdslave;
  if (pty(&fdmaster, &fdslave) == -1)
    return EXIT_FAILURE;

  if (start(envp, fdmaster, fdslave) == -1)
    return EXIT_FAILURE;

  return EXIT_SUCCESS;
}
