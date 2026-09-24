/* statics.c - the scenery, from the STATICS section of WL_ACT1.C.
 *
 * Plane 1 values 23..74 each name an entry in statinfo[]: the sprite to draw
 * and whether it is furniture you cannot walk through, a pickup, or just
 * dressing. The DOS table is written as a list of {picnum, type} with the
 * type left out for dressing, and #ifdef SPEAR changes a few entries and
 * adds four; here both tables are spelled out, and the data says which.
 */
#include "wolf.h"

statobj_t statobjlist[MAXSTATS];
int       numstats;

typedef struct {
    uint8_t picnum;                         /* SPR_STAT_n */
    uint8_t type;
} statinfo_t;

/* Wolfenstein's. Entry 48 is the second clip, which shares SPR_STAT_26. */
static const statinfo_t wolf_statinfo[] = {
    {  0, dressing       },  /* puddle           */
    {  1, block          },  /* green barrel     */
    {  2, block          },  /* table & chairs   */
    {  3, block          },  /* floor lamp       */
    {  4, dressing       },  /* chandelier       */
    {  5, block          },  /* hanged man       */
    {  6, bo_alpo        },  /* dog food         */
    {  7, block          },  /* red pillar       */
    {  8, block          },  /* tree             */
    {  9, dressing       },  /* skeleton         */
    { 10, block          },  /* sink             */
    { 11, block          },  /* potted plant     */
    { 12, block          },  /* urn              */
    { 13, block          },  /* bare table       */
    { 14, dressing       },  /* ceiling light    */
    { 15, dressing       },  /* kitchen stuff    */
    { 16, block          },  /* suit of armor    */
    { 17, block          },  /* hanging cage     */
    { 18, block          },  /* skeleton in cage */
    { 19, dressing       },  /* skeleton relax   */
    { 20, bo_key1        },  /* gold key         */
    { 21, bo_key2        },  /* silver key       */
    { 22, block          },  /* stuff            */
    { 23, dressing       },  /* stuff            */
    { 24, bo_food        },  /* good food        */
    { 25, bo_firstaid    },  /* first aid        */
    { 26, bo_clip        },  /* clip             */
    { 27, bo_machinegun  },  /* machine gun      */
    { 28, bo_chaingun    },  /* gatling gun      */
    { 29, bo_cross       },  /* cross            */
    { 30, bo_chalice     },  /* chalice          */
    { 31, bo_bible       },  /* bible            */
    { 32, bo_crown       },  /* crown            */
    { 33, bo_fullheal    },  /* one up           */
    { 34, bo_gibs        },  /* gibs             */
    { 35, block          },  /* barrel           */
    { 36, block          },  /* well             */
    { 37, block          },  /* empty well       */
    { 38, bo_gibs        },  /* gibs 2           */
    { 39, block          },  /* flag             */
    { 40, block          },  /* call apogee      */
    { 41, dressing       },  /* junk             */
    { 42, dressing       },  /* junk             */
    { 43, dressing       },  /* junk             */
    { 44, dressing       },  /* pots             */
    { 45, block          },  /* stove            */
    { 46, block          },  /* spears           */
    { 47, dressing       },  /* vines            */
    { 26, bo_clip2       },  /* clip, again      */
};

/* Spear of Destiny's: some of the same pictures are gibs to walk around,
 * and the marble pillar, the box of 25 rounds, the truck and the Spear
 * itself come before the second clip. */
static const statinfo_t spear_statinfo[] = {
    {  0, dressing       },  /* puddle           */
    {  1, block          },  /* green barrel     */
    {  2, block          },  /* table & chairs   */
    {  3, block          },  /* floor lamp       */
    {  4, dressing       },  /* chandelier       */
    {  5, block          },  /* hanged man       */
    {  6, bo_alpo        },  /* bad food         */
    {  7, block          },  /* red pillar       */
    {  8, block          },  /* tree             */
    {  9, dressing       },  /* skeleton flat    */
    { 10, block          },  /* gibs             */
    { 11, block          },  /* potted plant     */
    { 12, block          },  /* urn              */
    { 13, block          },  /* bare table       */
    { 14, dressing       },  /* ceiling light    */
    { 15, block          },  /* gibs             */
    { 16, block          },  /* suit of armor    */
    { 17, block          },  /* hanging cage     */
    { 18, block          },  /* skeleton in cage */
    { 19, dressing       },  /* skeleton relax   */
    { 20, bo_key1        },  /* gold key         */
    { 21, bo_key2        },  /* silver key       */
    { 22, block          },  /* gibs             */
    { 23, dressing       },  /* stuff            */
    { 24, bo_food        },  /* good food        */
    { 25, bo_firstaid    },  /* first aid        */
    { 26, bo_clip        },  /* clip             */
    { 27, bo_machinegun  },  /* machine gun      */
    { 28, bo_chaingun    },  /* gatling gun      */
    { 29, bo_cross       },  /* cross            */
    { 30, bo_chalice     },  /* chalice          */
    { 31, bo_bible       },  /* bible            */
    { 32, bo_crown       },  /* crown            */
    { 33, bo_fullheal    },  /* one up           */
    { 34, bo_gibs        },  /* gibs             */
    { 35, block          },  /* barrel           */
    { 36, block          },  /* well             */
    { 37, block          },  /* empty well       */
    { 38, bo_gibs        },  /* gibs 2           */
    { 39, block          },  /* flag             */
    { 40, dressing       },  /* red light        */
    { 41, dressing       },  /* junk             */
    { 42, dressing       },  /* junk             */
    { 43, dressing       },  /* junk             */
    { 44, block          },  /* gibs             */
    { 45, block          },  /* gibs             */
    { 46, block          },  /* gibs             */
    { 47, dressing       },  /* vines            */
    { 48, block          },  /* marble pillar    */
    { 49, bo_25clip      },  /* bonus 25 clip    */
    { 50, block          },  /* truck            */
    { 51, bo_spear       },  /* the Spear!       */
    { 26, bo_clip2       },  /* clip, again      */
};

static const statinfo_t *statinfo;
static int               num_stattypes;

/* SPR_STAT_48..51 are Spear of Destiny's own, numbered after Wolfenstein's
 * sprites (sprites.h) */
static int stat_sprite(int picnum)
{
    return picnum < 48 ? SPR_STAT_0 + picnum : SPR_STAT_48 + (picnum - 48);
}

void statics_init(void)
{
    numstats = 0;
    if (wolf_version == WV_SPEAR) {
        statinfo = spear_statinfo;
        num_stattypes = (int)(sizeof spear_statinfo / sizeof spear_statinfo[0]);
    } else {
        statinfo = wolf_statinfo;
        num_stattypes = (int)(sizeof wolf_statinfo / sizeof wolf_statinfo[0]);
    }
}

/* PlaceItemType(): what a dying enemy drops. The first statinfo[] entry
 * with that pickup type supplies the sprite, and the item goes in the
 * first free slot of the list - or on the end. It never blocks. */
void place_item_type(int itemtype, int tilex, int tiley)
{
    statobj_t *spot = NULL;
    int type;

    for (type = 0; type < num_stattypes; type++)
        if (statinfo[type].type == itemtype)
            break;
    if (type == num_stattypes)
        return;                             /* DOS quits here */

    for (int i = 0; i < numstats; i++)
        if (statobjlist[i].shapenum == -1) {
            spot = &statobjlist[i];
            break;
        }
    if (!spot) {
        if (numstats == MAXSTATS)
            return;                         /* no free spots */
        spot = &statobjlist[numstats++];
    }

    spot->shapenum   = stat_sprite(statinfo[type].picnum);
    spot->tilex      = tilex;
    spot->tiley      = tiley;
    spot->itemnumber = itemtype;
}

void spawn_static(int tilex, int tiley, int type)
{
    statobj_t *s;
    int item;

    /* Wolfenstein's table ends at the second clip, 48; tiles past that
     * name Spear of Destiny's pillar, truck and spear, which it has no
     * artwork for, and no map of it uses. */
    if (type < 0 || type >= num_stattypes || numstats == MAXSTATS)
        return;

    item = statinfo[type].type;

    s = &statobjlist[numstats++];
    s->tilex      = tilex;
    s->tiley      = tiley;
    s->shapenum   = stat_sprite(statinfo[type].picnum);
    s->itemnumber = item;

    if (item == block)
        blockmap[tiley][tilex] = 1;

    /* The five treasures are what the end-of-floor percentage counts. */
    if (item >= bo_cross && item <= bo_crown)
        gamestate.treasuretotal++;
    else if (item == bo_fullheal)
        gamestate.treasuretotal++;
}
