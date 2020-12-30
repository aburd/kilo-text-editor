/*** includes ***/

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <string.h>


/*** defines ***/
#define KILO_VERSION "0.0.1"
#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
  ARROW_UP = 'w',
  ARROW_DOWN = 's',
  ARROW_LEFT = 'a',
  ARROW_RIGHT = 'd',
};

/*** data ***/

struct editorConfig {
  int cx, cy;
  int screenrows;
  int screencols;
  struct termios orig_termios;
};

struct editorConfig E;

/*** terminal ***/

void die(const char *s) {
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);
  
  perror(s);
  exit(1);
}

void disableRawMode() {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
    die("tcsetattr");
}

void enableRawMode() {
  if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
  atexit(disableRawMode);

  struct termios raw = E.orig_termios;
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_iflag &= ~(IXON | ICRNL);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag |= (CS8);
  raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

char editorReadKey() {
  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN) die("read");
  }

  if (c == '\x1b') {
    char seq[3];

    if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

    if (seq[0] == '[') {
      switch (seq[1]) {
        case 'A': return ARROW_UP;
        case 'B': return ARROW_DOWN;
        case 'C': return ARROW_RIGHT;
        case 'D': return ARROW_LEFT;
      }
    }

    return '\x1b';
  }

  return c;
}

int getCursorPosition(int *rows, int *cols) {
  char buf[32];
  unsigned int i = 0;

  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

  while (i < sizeof(buf) - 1) {
    if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
    if (buf[i] == 'R') break;
    i++;
  }

  buf[i] = '\0';

  printf("\r\nbuf[2]: '%s'\r\n", &buf[2]);

  if (buf[0] != '\x1b' || buf[1] != '[') return -1;
  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

  return 0;
}

int getWindowSize(int *rows, int *cols) {
  struct winsize ws;

  if ((ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1) || ws.ws_col == 0) {
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
    return getCursorPosition(rows, cols);
  }
  *cols = ws.ws_col;
  *rows = ws.ws_row;
  return 0;
}

void initEditor() {
  E.cx = 0;
  E.cy = 0;

  if (getWindowSize(&E.screenrows, &E.screencols) == -1)
    die("getWindowSize");
}


/*** input ***/

void editorMoveCursor(char key) {
  switch (key) {
    case 'w':
      if (E.cy > 0) E.cy--;
      break;
    case 'a':
      if (E.cx > 0) E.cx--;
      break;
    case 'd':
      if (E.cx < E.screencols) E.cx++;
      break;
    case 's':
      if (E.cy < E.screenrows) E.cy++;
      break;
  }
}

void editorProcessKeypress() {
  char c = editorReadKey();
  switch (c)
  {
    case CTRL_KEY('c'):
      write(STDOUT_FILENO, "\x1b[2J", 4);
      write(STDOUT_FILENO, "\x1b[H", 3);
      exit(0);
      break;
    case 'w':
    case 'a':
    case 'd':
    case 's':
      editorMoveCursor(c);
      break;
  }
}

/*** append buffer ***/

struct abuf {
  char *b;
  int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len) {
  char *new = realloc(ab->b, ab->len + len);

  if (new == NULL) return;
  memcpy(&new[ab->len], s, len);
  ab->b = new;
  ab->len += len;
}

void abFree(struct abuf *ab) {
  free(ab->b);
}

/*** output ***/

void editorDrawRows(struct abuf *ab) {
  for (int y = 0; y < E.screenrows; y++) {
    abAppend(ab, "~", 1);

    // Write the title
    if (y == E.screenrows / 3) {
      char welcome[80];
      int welcomelen = snprintf(welcome, sizeof(welcome), 
        "~ Kilo Editor -- Version %s", KILO_VERSION);
      if (welcomelen > E.screencols) welcomelen = E.screencols;

      int padding = (E.screencols - welcomelen) / 2;
      if (padding) padding -= 1;
      while (padding--) abAppend(ab, " ", 1);

      abAppend(ab, welcome, welcomelen);
    }
    // Write debug
    if (y == E.screenrows / 3 + 1) {
      char debug[80];
      int debuglen = snprintf(debug, sizeof(debug), 
        "~ cx: %d, cy: %d", E.cx, E.cy);
      if (debuglen > E.screencols) debuglen = E.screencols;

      int padding = (E.screencols - debuglen) / 2;
      if (padding) padding -= 1;
      while (padding--) abAppend(ab, " ", 1);

      abAppend(ab, debug, debuglen);
    }
    
    // clear any text after the buffer
    abAppend(ab, "\x1b[K", 3);

    // Only add last line break if it isn't the last line
    if (y < E.screenrows - 1) {
      abAppend(ab, "\r\n", 2);
    }
  }
}

void editorRefreshScreen() {
  struct abuf ab = ABUF_INIT;

  // Hide cursor
  abAppend(&ab, "\x1b[?25l", 6);
    abAppend(&ab, "\x1b[H", 3);

  editorDrawRows(&ab);

  // Set position of cursor
  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy + 1, E.cx + 1);
  abAppend(&ab, buf, strlen(buf));

  // Show cursor
  abAppend(&ab, "\x1b[?25h", 6);

  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

/*** init ***/

int main() {
  enableRawMode();
  initEditor();

  while (1) {
    editorRefreshScreen();
    editorProcessKeypress();
  }

  return 0;
}
