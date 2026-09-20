#include <Arduino.h>
#include <stdbool.h>
#include <string.h>

/* ---------- Pin definitions ---------- */
#define DIN_PIN 11
#define CLK_PIN 13
#define CS_PIN  10

#define BTN_LEFT_PIN   2
#define BTN_RIGHT_PIN  3
#define BTN_ROTATE_PIN 4
#define BTN_DROP_PIN   5

/* ---------- MAX7219 registers ---------- */
#define MAX7219_REG_NOOP        0x00
#define MAX7219_REG_DECODEMODE  0x09
#define MAX7219_REG_INTENSITY   0x0A
#define MAX7219_REG_SCANLIMIT   0x0B
#define MAX7219_REG_SHUTDOWN    0x0C
#define MAX7219_REG_DISPLAYTEST 0x0F

#define FALL_INTERVAL   700UL
#define DEBOUNCE_DELAY  500UL

/* ---------- Game state ---------- */
static unsigned long lastFallTime = 0;
static byte board[8] = {0}; /* each byte = one row, 1=filled, 0=empty */

static int currentPiece = 0;
static int currentRotation = 0;
static int currentX = 0;
static int currentY = 0;
static bool gameOver = false;

typedef struct {
  byte shapes[4][4]; /* 4 rotations, each 4 rows of 4-bit patterns */
  int size;          /* bounding box size */
} Tetromino;

static const Tetromino tetrominoes[7] = {
  /* I */
  {
    {
      {B0000, B1111, B0000, B0000},
      {B0010, B0010, B0010, B0010},
      {B0000, B1111, B0000, B0000},
      {B0010, B0010, B0010, B0010}
    }, 4
  },
  /* O */
  {
    {
      {B0110, B0110, B0000, B0000},
      {B0110, B0110, B0000, B0000},
      {B0110, B0110, B0000, B0000},
      {B0110, B0110, B0000, B0000}
    }, 2
  },
  /* T */
  {
    {
      {B0100, B1110, B0000, B0000},
      {B0100, B0110, B0100, B0000},
      {B0000, B1110, B0100, B0000},
      {B0100, B1100, B0100, B0000}
    }, 3
  },
  /* S */
  {
    {
      {B0110, B1100, B0000, B0000},
      {B0100, B0110, B0010, B0000},
      {B0110, B1100, B0000, B0000},
      {B0100, B0110, B0010, B0000}
    }, 3
  },
  /* Z */
  {
    {
      {B1100, B0110, B0000, B0000},
      {B0010, B0110, B0100, B0000},
      {B1100, B0110, B0000, B0000},
      {B0010, B0110, B0100, B0000}
    }, 3
  },
  /* J */
  {
    {
      {B1000, B1110, B0000, B0000},
      {B0110, B0100, B0100, B0000},
      {B0000, B1110, B0010, B0000},
      {B0100, B0100, B1100, B0000}
    }, 3
  },
  /* L */
  {
    {
      {B0010, B1110, B0000, B0000},
      {B0100, B0100, B0110, B0000},
      {B0000, B1110, B1000, B0000},
      {B1100, B0100, B0100, B0000}
    }, 3
  }
};

static unsigned long lastPressTimes[20] = {0};

/* ---------- Forward declarations (required in plain C, no auto-prototyping) ---------- */
void max7219_write(byte reg, byte data);
void max7219_init(void);
void max7219_clear(void);
void max7219_set_row(byte row, byte value);

bool isButtonPressed(int pin);
void resetGame(void);
void showGameOver(void);
void spawnNewPiece(void);
void draw(void);
bool collision(int x, int y, int rotation);
bool tryMove(int x, int y, int rotation);
void tryRotateWithWallKick(void);
void placePiece(void);
void clearLines(void);
void dropPiece(void);

/* ---------- MAX7219 driver (replaces LedControl library) ---------- */
void max7219_write(byte reg, byte data) {
  digitalWrite(CS_PIN, LOW);
  shiftOut(DIN_PIN, CLK_PIN, MSBFIRST, reg);
  shiftOut(DIN_PIN, CLK_PIN, MSBFIRST, data);
  digitalWrite(CS_PIN, HIGH);
}

void max7219_init(void) {
  pinMode(DIN_PIN, OUTPUT);
  pinMode(CLK_PIN, OUTPUT);
  pinMode(CS_PIN, OUTPUT);
  digitalWrite(CS_PIN, HIGH);

  max7219_write(MAX7219_REG_DECODEMODE, 0x00);  /* no BCD decode, raw bits */
  max7219_write(MAX7219_REG_SCANLIMIT, 0x07);   /* show all 8 rows */
  max7219_write(MAX7219_REG_INTENSITY, 0x0C);   /* brightness ~ matches lc.setIntensity(0,12) */
  max7219_write(MAX7219_REG_SHUTDOWN, 0x01);    /* normal operation (not shutdown) */
  max7219_write(MAX7219_REG_DISPLAYTEST, 0x00); /* test mode off */

  max7219_clear();
}

void max7219_clear(void) {
  byte i;
  for (i = 1; i <= 8; i++) {
    max7219_write(i, 0x00);
  }
}

void max7219_set_row(byte row, byte value) {
  /* row is 0-7, MAX7219 digit registers are 1-8 */
  max7219_write(row + 1, value);
}

/* ---------- Input handling ---------- */
bool isButtonPressed(int pin) {
  unsigned long now = millis();
  if (digitalRead(pin) == LOW) {
    if (now - lastPressTimes[pin] > DEBOUNCE_DELAY) {
      lastPressTimes[pin] = now;
      return true;
    }
  }
  return false;
}

/* ---------- Game logic ---------- */
void resetGame(void) {
  memset(board, 0, sizeof(board));
  gameOver = false;
  spawnNewPiece();
}

void showGameOver(void) {
  int i;
  for (i = 0; i < 4; i++) {
    max7219_clear();
    delay(300);
    /* light up a 4x4 square in the middle (rows 2-5, cols 2-5) */
    max7219_set_row(2, B00111100);
    max7219_set_row(3, B00111100);
    max7219_set_row(4, B00111100);
    max7219_set_row(5, B00111100);
    delay(300);
  }
}

void spawnNewPiece(void) {
  currentPiece = random(0, 7);
  currentRotation = 0;
  currentX = 2;
  currentY = 0;
  if (collision(currentX, currentY, currentRotation)) {
    gameOver = true;
  }
}

void draw(void) {
  byte displayBuf[8];
  int r, c;
  const Tetromino *t = &tetrominoes[currentPiece];

  memcpy(displayBuf, board, sizeof(displayBuf));

  for (r = 0; r < t->size; r++) {
    byte rowBits = t->shapes[currentRotation][r];
    int py = currentY + r;
    if (py < 0 || py >= 8) continue;
    for (c = 0; c < 4; c++) {
      if (bitRead(rowBits, 3 - c)) {
        int px = currentX + c;
        if (px >= 0 && px < 8) {
          displayBuf[py] |= (1 << (7 - px));
        }
      }
    }
  }

  for (r = 0; r < 8; r++) {
    max7219_set_row(r, displayBuf[r]);
  }
}

bool collision(int x, int y, int rotation) {
  const Tetromino *t = &tetrominoes[currentPiece];
  int r, c;
  for (r = 0; r < t->size; r++) {
    byte rowBits = t->shapes[rotation][r];
    for (c = 0; c < 4; c++) {
      if (bitRead(rowBits, 3 - c)) {
        int px = x + c;
        int py = y + r;
        if (px < 0 || px >= 8 || py >= 8) return true;
        if (py >= 0 && (board[py] & (1 << (7 - px)))) return true;
      }
    }
  }
  return false;
}

bool tryMove(int x, int y, int rotation) {
  if (!collision(x, y, rotation)) {
    currentX = x;
    currentY = y;
    currentRotation = rotation;
    return true;
  }
  return false;
}

void tryRotateWithWallKick(void) {
  int newRotation = (currentRotation + 1) % 4;
  if (!collision(currentX, currentY, newRotation)) {
    currentRotation = newRotation;
    return;
  }
  if (!collision(currentX - 1, currentY, newRotation)) {
    currentX--;
    currentRotation = newRotation;
    return;
  }
  if (!collision(currentX + 1, currentY, newRotation)) {
    currentX++;
    currentRotation = newRotation;
    return;
  }
  /* rotation not possible, do nothing */
}

void placePiece(void) {
  const Tetromino *t = &tetrominoes[currentPiece];
  int r, c;
  for (r = 0; r < t->size; r++) {
    byte rowBits = t->shapes[currentRotation][r];
    for (c = 0; c < 4; c++) {
      if (bitRead(rowBits, 3 - c)) {
        int px = currentX + c;
        int py = currentY + r;
        if (px >= 0 && px < 8 && py >= 0 && py < 8) {
          board[py] |= (1 << (7 - px));
        }
      }
    }
  }
}

void clearLines(void) {
  int r, rr;
  for (r = 0; r < 8; r++) {
    if (board[r] == B11111111) {
      for (rr = r; rr > 0; rr--) {
        board[rr] = board[rr - 1];
      }
      board[0] = 0;
    }
  }
}

void dropPiece(void) {
  while (tryMove(currentX, currentY + 1, currentRotation)) {
    currentY++;
  }
  placePiece();
  clearLines();
  spawnNewPiece();
}

/* ---------- Arduino entry points ---------- */
void setup(void) {
  max7219_init();

  pinMode(BTN_LEFT_PIN, INPUT_PULLUP);
  pinMode(BTN_RIGHT_PIN, INPUT_PULLUP);
  pinMode(BTN_ROTATE_PIN, INPUT_PULLUP);
  pinMode(BTN_DROP_PIN, INPUT_PULLUP);

  randomSeed(analogRead(0));

  resetGame();
}

void loop(void) {
  if (gameOver) {
    showGameOver();
    delay(2500);
    resetGame();
    return;
  }

  if (isButtonPressed(BTN_LEFT_PIN))   tryMove(currentX - 1, currentY, currentRotation);
  if (isButtonPressed(BTN_RIGHT_PIN))  tryMove(currentX + 1, currentY, currentRotation);
  if (isButtonPressed(BTN_ROTATE_PIN)) tryRotateWithWallKick();
  if (isButtonPressed(BTN_DROP_PIN))   dropPiece();

  if (millis() - lastFallTime > FALL_INTERVAL) {
    lastFallTime = millis();
    if (!tryMove(currentX, currentY + 1, currentRotation)) {
      placePiece();
      clearLines();
      spawnNewPiece();
    }
  }

  draw();
}