/*** includes ***/
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <string.h>

/*** defines ***/
#define KILO_VERSION "0.0.1"
#define CTRL_KEY(k) ((k)&0x1f)
#define KILO_TAB_STOP 8

enum editorKey
{
  ARROW_UP = 1000,
  ARROW_DOWN,
  ARROW_LEFT,
  ARROW_RIGHT,
  PAGE_UP,
  PAGE_DOWN,
  HOME_KEY,
  END_KEY,
  DEL_KEY,
};

/*** data ***/

typedef struct erow
{
  int size;
  int rsize;
  int marginlsize;
  char *chars;
  char *render;
  char *marginl;
} erow;

struct editorConfig
{
  int cx, cy;
  int rx;
  int screenrows;
  int screencols;
  struct termios orig_termios;
  int rowoff;
  int coloff;
  int numrows;
  erow *row;
};

struct editorConfig E;

/*** terminal ***/

void die(const char *s)
{
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);

  perror(s);
  exit(1);
}

void disableRawMode()
{
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
    die("tcsetattr");
}

void enableRawMode()
{
  if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1)
    die("tcgetattr");
  atexit(disableRawMode);

  struct termios raw = E.orig_termios;
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_iflag &= ~(IXON | ICRNL);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag |= (CS8);
  raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
    die("tcsetattr");
}

int editorReadKey()
{
  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1)
  {
    if (nread == -1 && errno != EAGAIN)
      die("read");
  }

  if (c == '\x1b')
  {
    char seq[3];

    if (read(STDIN_FILENO, &seq[0], 1) != 1)
      return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1)
      return '\x1b';

    if (seq[0] == '[')
    {
      if (seq[1] >= '0' && seq[1] <= '9')
      {
        if (read(STDIN_FILENO, &seq[2], 1) != 1)
          return '\x1b';
        if (seq[2] == '~')
        {
          switch (seq[1])
          {
          case '1':
            return HOME_KEY;
          case '3':
            return DEL_KEY;
          case '4':
            return END_KEY;
          case '5':
            return PAGE_UP;
          case '6':
            return PAGE_DOWN;
          case '7':
            return HOME_KEY;
          case '8':
            return END_KEY;
          }
        }
      }
      else
      {
        switch (seq[1])
        {
        case 'A':
          return ARROW_UP;
        case 'B':
          return ARROW_DOWN;
        case 'C':
          return ARROW_RIGHT;
        case 'D':
          return ARROW_LEFT;
        case 'F':
          return END_KEY;
        case 'H':
          return HOME_KEY;
        }
      }
    }
    if (seq[0] == 'O')
    {
      switch (seq[1])
      {
      case 'F':
        return END_KEY;
      case 'H':
        return HOME_KEY;
      }
    }

    return '\x1b';
  }

  return c;
}

int getCursorPosition(int *rows, int *cols)
{
  char buf[32];
  unsigned int i = 0;

  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
    return -1;

  while (i < sizeof(buf) - 1)
  {
    if (read(STDIN_FILENO, &buf[i], 1) != 1)
      break;
    if (buf[i] == 'R')
      break;
    i++;
  }

  buf[i] = '\0';

  printf("\r\nbuf[2]: '%s'\r\n", &buf[2]);

  if (buf[0] != '\x1b' || buf[1] != '[')
    return -1;
  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2)
    return -1;

  return 0;
}

int getWindowSize(int *rows, int *cols)
{
  struct winsize ws;

  if ((ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1) || ws.ws_col == 0)
  {
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
      return -1;
    return getCursorPosition(rows, cols);
  }
  *cols = ws.ws_col;
  *rows = ws.ws_row;
  return 0;
}

void initEditor()
{
  E.numrows = 0;
  E.cx = 0;
  E.cy = 0;
  E.rx = 0;
  E.row = NULL;
  E.rowoff = 0;
  E.coloff = 0;

  if (getWindowSize(&E.screenrows, &E.screencols) == -1)
    die("getWindowSize");
}

/*** row operations ***/

int cxToRx(erow *row, int cx)
{
  int rx = row->marginlsize;
  int j;
  for (j = 0; j < cx; j++)
  {
    if (row->chars[j] == '\t')
    {
      rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
    }
    rx++;
  }

  return rx;
}

void editorUpdateRow(erow *row)
{
  // calculate how many tabs are in the row
  int tabs = 0;
  int j;
  for (j = 0; j < row->size; j++)
    if (row->chars[j] == '\t')
      tabs++;

  // allocate mem
  free(row->render);
  row->render = malloc(row->size + tabs * (KILO_TAB_STOP - 1) + 1 + row->marginlsize);

  // render margin
  int idx = 0;
  for (j = 0; j < row->marginlsize; j++)
  {
    row->render[idx++] = row->marginl[j];
  }

  // render chars
  for (j = 0; j < row->size; j++)
  {
    if (row->chars[j] == '\t')
    {
      row->render[idx++] = ' ';
      while (idx % KILO_TAB_STOP != 0)
        row->render[idx++] = ' ';
    }
    else
    {
      row->render[idx++] = row->chars[j];
    }
  }
  row->render[idx] = '\0';
  row->rsize = idx;
}

void editorAppendRow(char *s, size_t len)
{
  E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));

  int at = E.numrows;

  E.row[at].size = len;
  E.row[at].chars = malloc(len + 1);
  memcpy(E.row[at].chars, s, len);
  E.row[at].chars[len] = '\0';

  E.row[at].marginl = "~ ";
  E.row[at].marginlsize = 2;

  E.row[at].rsize = 0;
  E.row[at].render = NULL;
  editorUpdateRow(&E.row[at]);

  E.numrows++;
}

/*** file i/o ***/

void editorOpen(char *filename)
{
  FILE *fp = fopen(filename, "r");
  if (!fp)
    die("editorOpen");

  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen;

  while ((linelen = getline(&line, &linecap, fp)) != -1)
  {
    while (linelen > 0 && (line[linelen - 1] == '\r' ||
                           line[linelen - 1] == '\n'))
      linelen--;
    editorAppendRow(line, linelen);
  }
  free(line);
  fclose(fp);
}

/*** input ***/

void editorMoveCursor(int key)
{
  erow *row = (E.cy > E.numrows) ? NULL : &E.row[E.cy];
  switch (key)
  {
  case ARROW_UP:
    if (E.cy > 0)
      E.cy--;
    break;
  case ARROW_LEFT:
    if (E.cx > 0)
    {
      E.cx--;
    }
    else if (E.cx == 0 && E.cy > 0)
    {
      E.cy--;
      E.cx = E.row[E.cy].size;
    }
    break;
  case ARROW_RIGHT:
    if (row)
    {
      if (E.cx < row->size)
      {
        E.cx++;
      }
      else if (row && E.cx == row->size)
      {
        E.cy++;
        E.cx = 0;
      }
    }
    break;
  case ARROW_DOWN:
    if (E.cy <= E.numrows)
      E.cy++;
    break;
  case PAGE_DOWN:
  case PAGE_UP:
  {
    int times = E.screenrows;
    while (times--)
    {
      editorMoveCursor(key == PAGE_DOWN ? ARROW_DOWN : ARROW_UP);
    }
  }
  break;
  case HOME_KEY:
    E.cx = 0;
    break;
  case END_KEY:
    if (E.cy < E.numrows)
      E.cx = row->size;
    break;
  }

  row = (E.cy > E.numrows) ? NULL : &E.row[E.cy];
  int rowlen = row ? row->size : 0;
  if (E.cx > rowlen)
  {
    E.cx = rowlen;
  }
}

void editorProcessKeypress()
{
  int c = editorReadKey();
  switch (c)
  {
  case CTRL_KEY('c'):
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    exit(0);
    break;
  case ARROW_UP:
  case ARROW_DOWN:
  case ARROW_LEFT:
  case ARROW_RIGHT:
  case PAGE_UP:
  case PAGE_DOWN:
  case HOME_KEY:
  case END_KEY:
    editorMoveCursor(c);
    break;
  }
}

/*** append buffer ***/

struct abuf
{
  char *b;
  int len;
};

#define ABUF_INIT \
  {               \
    NULL, 0       \
  }

void abAppend(struct abuf *ab, const char *s, int len)
{
  char *new = realloc(ab->b, ab->len + len);

  if (new == NULL)
    return;
  memcpy(&new[ab->len], s, len);
  ab->b = new;
  ab->len += len;
}

void abFree(struct abuf *ab)
{
  free(ab->b);
}

/*** output ***/

void editorScroll()
{
  E.rx = 0;
  if (E.cy < E.numrows)
  {
    E.rx = cxToRx(&E.row[E.cy], E.cx);
  }

  if (E.cy < E.rowoff)
  {
    E.rowoff = E.cy;
  }
  if (E.cy >= E.rowoff + E.screenrows)
  {
    E.rowoff = E.cy - E.screenrows + 1;
  }
  if (E.rx < E.coloff)
  {
    E.coloff = E.rx;
  }
  if (E.rx >= E.coloff + E.screencols)
  {
    E.coloff = E.rx - E.screencols + 1;
  }
}

void drawTitle(struct abuf *ab)
{
  char welcome[80];
  int welcomelen = snprintf(welcome, sizeof(welcome),
                            "~ Kilo Editor -- Version %s", KILO_VERSION);
  if (welcomelen > E.screencols)
    welcomelen = E.screencols;

  int padding = (E.screencols - welcomelen) / 2;
  if (padding)
    padding -= 1;
  while (padding--)
    abAppend(ab, " ", 1);

  abAppend(ab, welcome, welcomelen);
}

void drawDebug(struct abuf *ab)
{
  char debug[80];
  int debuglen = snprintf(debug, sizeof(debug),
                          "~ cx: %d, cy: %d", E.cx, E.cy);
  if (debuglen > E.screencols)
    debuglen = E.screencols;

  int padding = (E.screencols - debuglen) / 2;
  if (padding)
    padding -= 1;
  while (padding--)
    abAppend(ab, " ", 1);

  abAppend(ab, debug, debuglen);
}

void editorDrawRows(struct abuf *ab)
{
  for (int y = 0; y < E.screenrows; y++)
  {
    int filerow = y + E.rowoff;
    if (filerow >= E.numrows)
    {
      // no file is opened
      if (E.numrows == 0)
      {
        if (y == E.screenrows / 3)
          drawTitle(ab);
        if (y == E.screenrows / 3 + 1)
          drawDebug(ab);
      }
    }
    else
    {
      int len = E.row[filerow].rsize - E.coloff;
      if (len < 0)
        len = 0;
      if (len > E.screencols)
        len = E.screencols;
      abAppend(ab, &E.row[filerow].render[E.coloff], len);
    }

    // clear any text after the buffer
    abAppend(ab, "\x1b[K", 3);

    // Only add last line break if it isn't the last line
    if (y < E.screenrows - 1)
    {
      abAppend(ab, "\r\n", 2);
    }
  }
}

void editorRefreshScreen()
{
  struct abuf ab = ABUF_INIT;

  // Hide cursor
  abAppend(&ab, "\x1b[?25l", 6);
  abAppend(&ab, "\x1b[H", 3);

  editorDrawRows(&ab);

  // Set position of cursor
  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, (E.rx - E.coloff) + 1);
  abAppend(&ab, buf, strlen(buf));

  // Show cursor
  abAppend(&ab, "\x1b[?25h", 6);

  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

/*** init ***/

int main(int argc, char *argv[])
{
  enableRawMode();
  initEditor();
  if (argc >= 2)
  {
    char *filename = argv[1];
    editorOpen(filename);
  }

  while (1)
  {
    editorScroll();
    editorRefreshScreen();
    editorProcessKeypress();
  }

  return 0;
}
