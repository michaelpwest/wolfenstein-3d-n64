/* controls.c - PollControls() for the pad: what the buttons, the D-pad and
 * the stick do, as Customize controls has set them (menu.c keeps the
 * settings).
 *
 * main.c only reads the hardware into a pad_state_t; everything from there
 * on is here, free of libdragon, so tools/sim can check it.
 *
 * The stick's four directions are the D-pad's four inputs. A direction whose
 * input has a move - turning, walking or strafing - moves in proportion to
 * how far the stick leans, as the stick always did, scaled by Stick
 * Sensitivity; a direction whose input has anything else counts as
 * pressed once the stick is past halfway, as it does in the menus.
 */
#include "wolf.h"

#define STICK_FULL   72     /* a worn stick may only reach 60; 72 is full */
#define STICK_DEAD   8
#define STICK_PRESS  40     /* the menus' ci.dir */

static int clamp_stick(int v)
{
    return v > STICK_FULL ? STICK_FULL : v < -STICK_FULL ? -STICK_FULL : v;
}

/* How far a move goes this frame: all of delta if its input is a button or
 * the D-pad, held; in proportion if its input is a D-pad direction and the
 * stick leans that way; the larger of the two. Stick Sensitivity scales the
 * stick, as Mouse Sensitivity scaled both of the mouse's axes in DOS
 * (PollControls()), and never a button, as it never scaled a key. A turn
 * or a strafe may go past the D-pad's speed, up to 1.4 times it; walking
 * forward and back stops at it. */
static int move_amount(int action, unsigned buttons, const int lean[4],
                       int delta, bool sideways)
{
    const int in = ctl_button[action];
    int amount = (buttons >> in) & 1 ? delta : 0;

    if (in >= PAD_D_LEFT && lean[in - PAD_D_LEFT]) {
        int s = delta * lean[in - PAD_D_LEFT] * (ctl_stick_sensitivity + 5)
              / (STICK_FULL * 10);
        if (!sideways && s > delta)
            s = delta;
        if (s > amount)
            amount = s;
    }
    return amount;
}

void ctl_read_pad(const pad_state_t *p, int tics, controls_t *c, buttons_t *b)
{
    static unsigned stick_last;             /* its directions, as D-pad bits */
    static bool map_chord;                  /* L + R came while Map was down */
    const int sx = clamp_stick(p->stick_x), sy = clamp_stick(p->stick_y);
    int lean[4] = { 0, 0, 0, 0 };           /* toward left, right, up, down */
    unsigned stick = 0, held, hit, rel;
    int delta, horiz, fwd;

    if (sx < -STICK_DEAD) lean[0] = -sx;
    if (sx >  STICK_DEAD) lean[1] = sx;
    if (sy >  STICK_DEAD) lean[2] = sy;
    if (sy < -STICK_DEAD) lean[3] = -sy;
    for (int i = 0; i < 4; i++)
        if (lean[i] > STICK_PRESS)
            stick |= 1u << (PAD_D_LEFT + i);

    /* every input as held, pressed and released, the stick with the D-pad */
    held = p->held | stick;
    hit  = p->pressed | (stick & ~stick_last);
    rel  = p->released | (stick_last & ~stick);
    stick_last = stick;

    /* the Run button runs, or with Always Run on, walks */
    delta = (ctl_held(CTL_RUN, held) != ctl_always_run ? RUNMOVE : BASEMOVE) * tics;

    horiz = move_amount(CTL_RIGHT, p->held, lean, delta, true)
          - move_amount(CTL_LEFT, p->held, lean, delta, true);
    fwd   = move_amount(CTL_BACKWARD, p->held, lean, delta, false)
          - move_amount(CTL_FORWARD, p->held, lean, delta, false);

    c->turn = c->strafe = 0;
    if (ctl_held(CTL_STRAFE, held))
        c->strafe += horiz;                 /* bt_strafe, as in PollControls() */
    else
        c->turn += horiz;
    c->strafe += move_amount(CTL_STRAFE_RIGHT, p->held, lean, delta, true)
               - move_amount(CTL_STRAFE_LEFT, p->held, lean, delta, true);
    c->forward = fwd;

    /* L and R together pause (the DOS Pause key), whatever they do alone,
     * and only L and R again carry on, as in Doom; Start still opens the
     * menu. The Map input (L, unless customized) shows or hides the map.
     * So that L + R is a pause and not also a map, the map goes on the
     * input's release, and only when L and R have not been down together at
     * any point while it was. */
    {
        const bool l = (p->held >> PAD_L) & 1, r = (p->held >> PAD_R) & 1;
        b->pause = l && r && ((p->pressed >> PAD_L) & 1 || (p->pressed >> PAD_R) & 1);
        if (l && r && ctl_held(CTL_MAP, held))
            map_chord = true;
        b->map = ctl_held(CTL_MAP, rel) && !map_chord;
        if (!ctl_held(CTL_MAP, held))
            map_chord = false;
    }

    /* the menus' keys, which stay where they are */
    b->use        = (p->pressed >> PAD_A) & 1;
    b->menu       = p->start;
    b->back       = (p->pressed >> PAD_B) & 1;
    b->back_held  = (p->held >> PAD_B) & 1;
    b->left       = (hit >> PAD_D_LEFT) & 1;        /* the stick too, a push */
    b->right      = (hit >> PAD_D_RIGHT) & 1;       /* past halfway a press  */
    b->up         = (held >> PAD_D_UP) & 1;
    b->down       = (held >> PAD_D_DOWN) & 1;
    b->left_held  = (held >> PAD_D_LEFT) & 1;
    b->right_held = (held >> PAD_D_RIGHT) & 1;

    /* and the game's, wherever they have been put */
    b->use_held    = ctl_held(CTL_OPEN, held);
    b->fire        = ctl_held(CTL_FIRE, held);
    b->pad_pressed = hit;
    b->change      = ctl_held(CTL_NEXT_WEAPON, hit) ? 1
                   : ctl_held(CTL_PREV_WEAPON, hit) ? -1 : 0;
    b->select      = 0;
}
