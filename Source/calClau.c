#include <mega32.h>
#include <delay.h>
#include <alcd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#define R0  PINA.0
#define R1  PINA.1
#define R2  PINA.2
#define R3  PINA.3

// -- Limits --------------------------------
#define MAX_DIGITS  6       /* max significant DIGIT characters (0-9) per number,
                                not counting the sign or the decimal point */
#define BUF_LEN     (MAX_DIGITS + 4)  /* sign + digits + '.' + null (+1 spare) */
#define LCD_COLS    16

// -- States --------------------------------
#define ST_NUM1      0
#define ST_NUM2      1
#define ST_RESULT  2
//------------------------------------------------
#define KEY_CLEAR       'C'
#define KEY_SIGN          'S'       //   +/-   sign of current entry
#define KEY_PERCENT  'P'      //   %
#define KEY_SQRT         'r'       // sqrt(x)
#define KEY_MRC           'R'     // MRC   - memory recall / memory clear
#define KEY_MMINUS     'n'      // M-    - subtract entry from memory
#define KEY_MPLUS       'p'      // M+    - add entry to memory
#define KEY_EQUAL       '='
#define KEY_PLUS          '+'
#define KEY_MINUS        '-'
#define KEY_MUL            '*'
#define KEY_DIV             '/'
#define KEY_DECIMAL    '.'

// -- Column shift table -----------------------
flash char col_shift[6] = { 0xFE, 0xFD, 0xFB, 0xF7, 0xEF, 0xDF };//shifting a 0 on columns

// -- Keypad layout -----------------------------
flash char layout[24] = {
    KEY_CLEAR , '7', '8', '9', KEY_MUL  , KEY_DIV ,
    KEY_SIGN  , '4', '5', '6', KEY_MINUS, KEY_MRC ,
    KEY_PERCENT,'1', '2', '3', KEY_PLUS , KEY_MMINUS,
    KEY_SQRT  , '0', '.', KEY_EQUAL, KEY_PLUS, KEY_MPLUS
};

// -- Global calculator state -------------------
volatile char key = 0; // volatile cuz its in interrupt
int  state = ST_NUM1;
char op    = 0;
char buf1[BUF_LEN];
char buf2[BUF_LEN];
char disp[LCD_COLS + 1];

// -- Memory (EEPROM) ------------------------
eeprom float mem_value;
char mrc_stage = 0;   // 0 = next MRC press recalls, 1 = next MRC press clears

char  keypad(void);
float mathOp(float a, float b, char opr);
void  append_digit(char *buf, char c);
void  calc_clear(void);
void  show_buf(void);
void  toggle_sign(char *buf);
void  apply_percent(void);
void  apply_sqrt(void);
void  mem_recall_or_clear(void);
void  mem_add_current(void);
void  mem_sub_current(void);
char  has_decimal(char *buf);
void  format_number(char *dest, float val);

interrupt [EXT_INT2] void ext_int2_isr(void)
{
    key = keypad();
}

//==========================Main======================================
void main(void)
{
    char  k;
    char  *buf;
    float a, b, res;

    DDRA  = 0x00;
    PORTA = 0x0F;

    DDRB = 0x00;
    PORTB = 0x04;     // B2(interrupt)Pull-up

    DDRC  = 0x3F;
    PORTC = 0x00;

    lcd_init(16);
    lcd_clear();
    lcd_putsf("0");

    GICR   |= (1<<INT2);
    MCUCSR  = (0<<ISC2);
    GIFR   |= (1<<INTF2);

    calc_clear();

#asm("sei")

    while (1)
    {
        if (key == 0) continue; // Skip while loop until a key is pressed

        k   = key;
        key = 0;

        if (k != KEY_MRC) mrc_stage = 0;   /* any other key cancels MRC toggle
                                              Without this, pressing any key between two MRC presses
                                              would still treat the second MRC as the second press and clear memory*/

        // -- Digit -----------------------------------------------------------------------
        if (k >= '0' && k <= '9')
        {
            if (state == ST_RESULT)
                calc_clear();

            buf  = (state == ST_NUM2) ? buf2 : buf1;

            // Replace lone "0" or "-0" cleanly
            if (strcmp(buf, "0") == 0)
            {
                buf[0] = '\0';
            }
            if (strcmp(buf, "-0") == 0)
            {
                buf[0] = '-';
                buf[1] = '\0';
            }

           append_digit(buf, k);
            show_buf();
        }

        /* -- Decimal point -----------------------------------------------------------
               Adds a '.' to the active buffer (once). Starts a
               fresh "0." if nothing has been typed yet. -------- */
        else if (k == KEY_DECIMAL)
        {
            if (state == ST_RESULT)
                calc_clear();

            buf = (state == ST_NUM2) ? buf2 : buf1;

            if (strlen(buf) == 0)
            {
                strcpy(buf, "0.");
            }
            else if (strcmp(buf, "-") == 0)
            {
                strcat(buf, "0.");
            }
            else if (!has_decimal(buf))
            {
                strcat(buf, ".");
            }
            show_buf();
        }

        /* -- Leading minus starts a negative number --------------
               Pressing the '-' key while no first operand has been
               typed yet (fresh start, or right after a result) means
               "negative number", not "subtract". ------------------ */
        else if (k == KEY_MINUS && (state == ST_NUM1 || state == ST_RESULT)
                 && strlen(buf1) == 0)
        {
            if (state == ST_RESULT) calc_clear();
            buf1[0] = '-';
            buf1[1] = '\0';
            show_buf();
        }

        // -- Operator ------------------------------------------
        else if (k == KEY_PLUS || k == KEY_MINUS || k == KEY_MUL || k == KEY_DIV)
        {
            //Need a real number in buf1
            if (strlen(buf1) == 0)        continue;
            if (strcmp(buf1, "-") == 0)   continue;

            if (state == ST_NUM1 || state == ST_RESULT)
            {
                op      = k;
                state   = ST_NUM2;
                buf2[0] = '\0';
                lcd_clear();
                lcd_putchar(op);
            }
            else if (state == ST_NUM2)
            {
                if (strlen(buf2) == 0)
                {
                    // Change operator before any num2 typed
                    op = k;
                    lcd_clear();
                    lcd_putchar(op);
                }
                else
                {
                    // Chain: evaluate then set new operator
                    a   = atof(buf1);
                    b   = atof(buf2);
                    res = mathOp(a, b, op);

                    format_number(disp, res);
                    disp[LCD_COLS] = '\0';
                    lcd_clear();
                    lcd_puts(disp);
                    delay_ms(500);

                    strncpy(buf1, disp, BUF_LEN - 1);
                    buf1[BUF_LEN - 1] = '\0';
                    buf2[0] = '\0';
                    op    = k;
                    state = ST_NUM2;
                    lcd_clear();
                    lcd_putchar(op);
                }
            }
        }

        // -- Sign toggle (+/-) -----------------------------------
        else if (k == KEY_SIGN)
        {
            buf = (state == ST_NUM2) ? buf2 : buf1;
            toggle_sign(buf);
        }

        // -- Percent (%) -----------------------------------------
        else if (k == KEY_PERCENT)
        {
            apply_percent();
        }

        // -- Square root -------------------------------------------
        else if (k == KEY_SQRT)
        {
            apply_sqrt();
        }

        // -- Memory recall / clear (MRC) ----------------------------
        else if (k == KEY_MRC)
        {
            mem_recall_or_clear();
        }

        // -- Memory add (M+) -----------------------------------------
        else if (k == KEY_MPLUS)
        {
            mem_add_current();
        }

        // -- Memory subtract (M-) --------------------------------------
        else if (k == KEY_MMINUS)
        {
            mem_sub_current();
        }

        // -- Equals --------------------------------------------
        else if (k == KEY_EQUAL)
        {
            if (state != ST_NUM2)         continue;
            if (strlen(buf2) == 0)            continue;
            if (strcmp(buf2, "-") == 0)     continue;

            a   = atof(buf1);
            b   = atof(buf2);
            res = mathOp(a, b, op);

            format_number(disp, res);
            disp[LCD_COLS] = '\0';
            lcd_clear();
            lcd_puts(disp);

            strncpy(buf1, disp, BUF_LEN - 1);
            buf1[BUF_LEN - 1] = '\0';
            buf2[0] = '\0';
            op    = 0;
            state = ST_RESULT;
        }

        // -- Clear ---------------------------------------------
        else if (k == KEY_CLEAR)
        {
            calc_clear();
            lcd_clear();
            lcd_putsf("0");
        }
    }
}

//============================Functions===================================

//--------  keypad()----------------------------------------------------------
char keypad(void)
{
    int  row, col, pos;
    char found;

    found = 0;
    pos   = 0;

    for (col = 0; col < 6; col++)
    {
        PORTC = col_shift[col];
        delay_ms(5);

        row = -1;
        if      (R0 == 0) row = 0;
        else if (R1 == 0) row = 1;
        else if (R2 == 0) row = 2;
        else if (R3 == 0) row = 3;

        if (row != -1) // means if a row is found
        {
            pos   = row * 6 + col;
            found = 1;
            delay_ms(20);
            while (R0 == 0 || R1 == 0 || R2 == 0 || R3 == 0) {}
            delay_ms(20);
            break;    // when row is found, dont check the rest of the rows, exit the for loop
        }
    }

    PORTC = 0x00;   /*back to idle "ready" state
                                  this line can be written in iteruupt*/
    return found ? layout[pos] : 0;    //found = 0 means no key was detected so return 0
}

/* ------------------------------------------------------------------
 * mathOp()
 * ------------------------------------------------------------------ */
float mathOp(float a, float b, char opr)
{
    switch (opr)
    {
        case '+': return a + b;
        case '-': return a - b;
        case '*': return a * b;
        case '/':
            if (b == 0)
            {
                lcd_clear();
                lcd_putsf("Err: Div/0");
                delay_ms(1500);
                calc_clear();
                lcd_clear();
                lcd_putsf("0");
                return 0;
            }
            return a / b;
        /*default: return a;// mitoni bezary ke case ha halat
        defult ham dashte bashn valy in halat aslan etefagh nemi ofte*/
    }
}


/* ------------------------------------------------------------------------
 * has_decimal()
 * Returns nonzero if buf already contains a '.' character.
 * -------------------------------------------------------------------------*/
char has_decimal(char *buf)
{
    int i;
    for (i = 0; buf[i] != '\0'; i++)
    {
        if (buf[i] == '.') return 1;
    }
    return 0;
}

/* -------------------------------------------------------------------------------------
   format_number()
   Converts float to string for display.
   Removes trailing zeros and decimal point for whole numbers.
   "4.5" instead of "4.500000" and "4 instead of "4.000000"
  ---------------------------------------------------------------------------------------- */
void format_number(char *dest, float val)
{
    /* CVAVR sprintf on this project only has "int" enabled -- no %f
       support at all, so the fractional part is built by hand using
       plain %d / %s formatting only. */
    char  neg;
    int   ip;  //Integer Part
    int   frac;
    float fv;
    char  fracStr[4];
    int   len;

    // 1. Checking for negative sign
    neg = 0;
    if (val < 0) { neg = 1; val = -val; }


    // 2. Separating the integer and fractional parts
    ip = (int)val;            //   (e.g. 4.56 -> ip=4
    fv = val - (float)ip;   //    fv=0.56)


    // 3. Converting the fractional part to an integer (up to 3 digits)
    frac = (int)(fv * 1000.0 + 0.5);         // 0.56 -> 560 (rounded)

    // 4. If rounding causes an increase
    if (frac >= 1000) { frac = 0; ip++; }

    // 5. If there is no fractional part (frac == 0)
    if (frac == 0)
    {
        sprintf(dest, neg ? "-%d" : "%d", ip);   // Only "4" or "-4"
    }                               //if neg=1(negative number)->  -%d
    else
    {
        // 6. If it has a fractional part:
        sprintf(fracStr, "%03d", frac);     // "560"
        len = 3;

        // Removing extra zeros from the end
        while (len > 0 && fracStr[len - 1] == '0') { fracStr[--len] = '\0'; }
                                    /* is fracStr[2] = '0'? (560->fracStr[2]=0) yes  -->
                                                             len-2 ,    fracStr[2] = '\0'
                                                             and '\0' is the end of the string so 0 would be deleted*/

        sprintf(dest, neg ? "-%d.%s" : "%d.%s", ip, fracStr);
    }
}

/* ------------------------------------------------------------------
 * show_buf()
 * ------------------------------------------------------------------ */
void show_buf(void)
{
    lcd_clear();
    if (state == ST_NUM2)
    {
        disp[0] = op;
        disp[1] = '\0';
        strncat(disp, buf2, LCD_COLS - 1);
        disp[LCD_COLS] = '\0';
        lcd_puts(disp);
    }
    else
    {
        lcd_puts(buf1[0] ? buf1 : "0");
    }
}

/* -------------------------------------------------------------------------------------
 * append_digit()
 * Appends the newly entered digit to the end of the current input
 * ------------------------------------------------------------------------------------- */
void append_digit(char *buf, char c)
{
    int len;
    len = strlen(buf);
    if (len < BUF_LEN - 1)
    {
        buf[len]     = c;
        buf[len + 1] = '\0';
    }
}

/* ------------------------------------------------------------------
 * calc_clear()
 * ------------------------------------------------------------------ */
void calc_clear(void)
{
    buf1[0] = '\0';
    buf2[0] = '\0';
    op      = 0;
    state   = ST_NUM1;
}

/* ------------------------------------------------------------------
 * toggle_sign()
 * Flips the leading '-' of the given buffer, then refreshes the LCD.
 * If the buffer is still empty, this starts a fresh negative entry
 * (so the user can enter a negative number before typing any digit).
 * ------------------------------------------------------------------ */
void toggle_sign(char *buf)
{
    int len;

    len = strlen(buf);

    if (buf[0] == '-')
    {
        /* remove the leading minus */
        memmove(buf, buf + 1, len);   /* shifts the terminator too */
    }
    else if (len == 0)
    {
        /* nothing entered yet: start a negative number */
        buf[0] = '-';
        buf[1] = '\0';
    }
    else
    {
        if (len >= BUF_LEN - 2) return;   /* no room left for the sign */
        memmove(buf + 1, buf, len + 1);
        buf[0] = '-';
    }
    show_buf();
}

/* ------------------------------------------------------------------
 * apply_percent()
 * ------------------------------------------------------------------ */
void apply_percent(void)
{
    float a, b, res;
    char  *buf;

    if (state == ST_NUM2 && strlen(buf2) > 0 && strcmp(buf2, "-") != 0)
    {
        a   = atof(buf1);
        b   = atof(buf2);
        res = (a * b) / 100.0;
        format_number(buf2, res);
    }
    else
    {
        buf = (state == ST_NUM2) ? buf2 : buf1;
        if (strlen(buf) == 0 || strcmp(buf, "-") == 0) return;
        res = atof(buf) / 100.0;
        format_number(buf, res);
    }
    show_buf();
}

/* ------------------------------------------------------------------
 * apply_sqrt()
 * Square root of the active buffer (float precision).
 * ------------------------------------------------------------------ */
void apply_sqrt(void)
{
    float val, res;
    char  *buf;

    buf = (state == ST_NUM2) ? buf2 : buf1;
    if (strlen(buf) == 0 || strcmp(buf, "-") == 0) return;

    val = atof(buf);
    if (val < 0)
    {
        lcd_clear();
        lcd_putsf("Err: sqrt(-)");
        delay_ms(1500);
        show_buf();
        return;
    }

    res = sqrt(val);
    format_number(buf, res);
    show_buf();
}

/* ------------------------------------------------------------------
 * mem_recall_or_clear()
 * First MRC press after any other key   -> recall memory into buf1
 * Immediate second MRC press in a row   -> clear memory to 0
 * ------------------------------------------------------------------ */
void mem_recall_or_clear(void)
{
    if (mrc_stage == 0)
    {
        format_number(buf1, mem_value);
        buf2[0] = '\0';
        op      = 0;
        state   = ST_RESULT;
        lcd_clear();
        lcd_puts(buf1);
        mrc_stage = 1;
    }
    else
    {
        mem_value = 0;              /* EEPROM write */
        lcd_clear();
        lcd_putsf("Mem Clear");
        delay_ms(800);
        calc_clear();
        lcd_clear();
        lcd_putsf("0");
        mrc_stage = 0;
    }
}

/* ------------------------------------------------------------------
 * mem_add_current() / mem_sub_current()
 * Adds/subtracts the value currently shown on the display (buf1) to
 * / from the EEPROM memory register, with a short on-screen blip.
 * ------------------------------------------------------------------ */
void mem_add_current(void)
{
    float val;
    if (strlen(buf1) == 0 || strcmp(buf1, "-") == 0) return;
    val = atof(buf1);
    mem_value = mem_value + val;   /* EEPROM write */

    lcd_clear();
    lcd_putsf("M+");
    delay_ms(500);
    show_buf();
}

void mem_sub_current(void)
{
    float val;
    if (strlen(buf1) == 0 || strcmp(buf1, "-") == 0) return;
    val = atof(buf1);
    mem_value = mem_value - val;   /* EEPROM write */

    lcd_clear();
    lcd_putsf("M-");
    delay_ms(500);
    show_buf();
}