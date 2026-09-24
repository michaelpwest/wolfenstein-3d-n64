/* actors.c - the enemies.
 *
 * WL_STATE.C (movement, sight, damage) and WL_ACT2.C (every enemy, boss and
 * projectile of Wolfenstein 3D and Spear of Destiny, and the death cam),
 * with DoActor() from WL_PLAY.C and the player's GunAttack() and
 * KnifeAttack() from WL_AGENT.C. The DOS game was built once per game with
 * #ifdef SPEAR and #ifdef UPLOAD; here the data says which (wolf_version).
 * The state tables are the DOS ones, frame for frame; the think and action
 * routines follow the originals line by line, including their quirks, and
 * note where they had to change.
 *
 * Nothing here is N64-specific, so tools/sim runs all of it.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "wolf.h"

#define SPDPATROL       512
#define SPDDOG          1500
#define BJRUNSPEED      2048
#define BJJUMPSPEED     680
#define MINSIGHT        0x18000L
#define ICONARROWS      90
#define PROJECTILESIZE  0xc000L
#define PROJSIZE        0x2000
#define ANGLES          360                 /* the DOS game's own degrees */

/* The renderer's projection center and GunAttack()'s aiming slop: centerx
 * (viewwidth/2 - 1) and shootdelta (viewwidth/10) in SetViewSize(), so a
 * smaller view aims with fewer pixels of slop, as it did in DOS. */
#define CENTERX      (view_width / 2 - 1)
#define SHOOTDELTA   (view_width / 10)

/* GunAttack() and KnifeAttack() aim with ob->aimx, which during a demo is
 * placed in the 304-pixel view the demos are checked against. */
#define AIM_CENTER   (dos_exact ? 304 / 2 - 1 : CENTERX)
#define AIM_DELTA    (dos_exact ? 304 / 10 : SHOOTDELTA)

objtype  objlist[MAXACTORS];
uint8_t  obj_order[MAXACTORS];
int      numobjs;
objtype *actorat[MAPSIZE][MAPSIZE];
bool     victory_done;
bool     deathcam_started, deathcam;

static uint8_t free_slots[MAXACTORS];       /* a stack: the next is on top */
static int     num_free;

static int think_tics;                      /* the frame's tics, for thinks */

/* ------------------------------------------------------------------ */
/* State tables - WL_ACT2.C                                            */
/* ------------------------------------------------------------------ */

static void T_Stand(objtype *ob);
static void T_Path(objtype *ob);
static void T_Chase(objtype *ob);
static void T_DogChase(objtype *ob);
static void T_Ghosts(objtype *ob);
static void T_Shoot(objtype *ob);
static void T_Bite(objtype *ob);
static void T_Projectile(objtype *ob);
static void A_DeathScream(objtype *ob);
static void A_Smoke(objtype *ob);
static void T_BJRun(objtype *ob);
static void T_BJJump(objtype *ob);
static void T_BJYell(objtype *ob);
static void T_BJDone(objtype *ob);
static void T_Schabb(objtype *ob);
static void T_SchabbThrow(objtype *ob);
static void T_Gift(objtype *ob);
static void T_GiftThrow(objtype *ob);
static void T_Fat(objtype *ob);
static void T_Fake(objtype *ob);
static void T_FakeFire(objtype *ob);
static void A_Slurpie(objtype *ob);
static void A_HitlerMorph(objtype *ob);
static void A_MechaSound(objtype *ob);
static void A_StartDeathCam(objtype *ob);
static void T_Launch(objtype *ob);
static void T_Will(objtype *ob);
static void T_UShoot(objtype *ob);
static void A_Relaunch(objtype *ob);
static void A_Victory(objtype *ob);
static void A_StartAttack(objtype *ob);
static void A_Breathing(objtype *ob);
static void A_Dormant(objtype *ob);

/* forward declarations, so the tables can point ahead */
static const statetype s_grdstand, s_grdpath1, s_grdpath1s, s_grdpath2,
    s_grdpath3, s_grdpath3s, s_grdpath4, s_grdpain, s_grdpain1, s_grdshoot1,
    s_grdshoot2, s_grdshoot3, s_grdchase1, s_grdchase1s, s_grdchase2,
    s_grdchase3, s_grdchase3s, s_grdchase4, s_grddie1, s_grddie2, s_grddie3,
    s_grddie4;
static const statetype s_dogpath1, s_dogpath1s, s_dogpath2, s_dogpath3,
    s_dogpath3s, s_dogpath4, s_dogjump1, s_dogjump2, s_dogjump3, s_dogjump4,
    s_dogjump5, s_dogchase1, s_dogchase1s, s_dogchase2, s_dogchase3,
    s_dogchase3s, s_dogchase4, s_dogdie1, s_dogdie2, s_dogdie3, s_dogdead;
static const statetype s_ssstand, s_sspath1, s_sspath1s, s_sspath2,
    s_sspath3, s_sspath3s, s_sspath4, s_sspain, s_sspain1, s_ssshoot1,
    s_ssshoot2, s_ssshoot3, s_ssshoot4, s_ssshoot5, s_ssshoot6, s_ssshoot7,
    s_ssshoot8, s_ssshoot9, s_sschase1, s_sschase1s, s_sschase2, s_sschase3,
    s_sschase3s, s_sschase4, s_ssdie1, s_ssdie2, s_ssdie3, s_ssdie4;
static const statetype s_bossstand, s_bosschase1, s_bosschase1s,
    s_bosschase2, s_bosschase3, s_bosschase3s, s_bosschase4, s_bossdie1,
    s_bossdie2, s_bossdie3, s_bossdie4, s_bossshoot1, s_bossshoot2,
    s_bossshoot3, s_bossshoot4, s_bossshoot5, s_bossshoot6, s_bossshoot7,
    s_bossshoot8;
static const statetype s_bjrun1, s_bjrun1s, s_bjrun2, s_bjrun3, s_bjrun3s,
    s_bjrun4, s_bjjump1, s_bjjump2, s_bjjump3, s_bjjump4;

/* the full game's */
static const statetype s_rocket, s_smoke1, s_smoke2, s_smoke3, s_smoke4,
    s_boom1, s_boom2, s_boom3;
static const statetype s_ofcstand, s_ofcpath1, s_ofcpath1s, s_ofcpath2,
    s_ofcpath3, s_ofcpath3s, s_ofcpath4, s_ofcpain, s_ofcpain1, s_ofcshoot1,
    s_ofcshoot2, s_ofcshoot3, s_ofcchase1, s_ofcchase1s, s_ofcchase2,
    s_ofcchase3, s_ofcchase3s, s_ofcchase4, s_ofcdie1, s_ofcdie2, s_ofcdie3,
    s_ofcdie4, s_ofcdie5;
static const statetype s_mutstand, s_mutpath1, s_mutpath1s, s_mutpath2,
    s_mutpath3, s_mutpath3s, s_mutpath4, s_mutpain, s_mutpain1, s_mutshoot1,
    s_mutshoot2, s_mutshoot3, s_mutshoot4, s_mutchase1, s_mutchase1s,
    s_mutchase2, s_mutchase3, s_mutchase3s, s_mutchase4, s_mutdie1,
    s_mutdie2, s_mutdie3, s_mutdie4, s_mutdie5;
static const statetype s_blinkychase1, s_blinkychase2, s_inkychase1,
    s_inkychase2, s_pinkychase1, s_pinkychase2, s_clydechase1, s_clydechase2;
static const statetype s_gretelstand, s_gretelchase1, s_gretelchase1s,
    s_gretelchase2, s_gretelchase3, s_gretelchase3s, s_gretelchase4,
    s_greteldie1, s_greteldie2, s_greteldie3, s_greteldie4, s_gretelshoot1,
    s_gretelshoot2, s_gretelshoot3, s_gretelshoot4, s_gretelshoot5,
    s_gretelshoot6, s_gretelshoot7, s_gretelshoot8;
static const statetype s_schabbstand, s_schabbchase1, s_schabbchase1s,
    s_schabbchase2, s_schabbchase3, s_schabbchase3s, s_schabbchase4,
    s_schabbdeathcam, s_schabbdie1, s_schabbdie3, s_schabbdie4, s_schabbdie5,
    s_schabbdie6, s_schabbshoot1, s_schabbshoot2, s_needle1, s_needle2,
    s_needle3, s_needle4;
static const statetype s_giftstand, s_giftchase1, s_giftchase1s,
    s_giftchase2, s_giftchase3, s_giftchase3s, s_giftchase4, s_giftdeathcam,
    s_giftdie1, s_giftdie3, s_giftdie4, s_giftdie5, s_giftdie6, s_giftshoot1,
    s_giftshoot2;
static const statetype s_fatstand, s_fatchase1, s_fatchase1s, s_fatchase2,
    s_fatchase3, s_fatchase3s, s_fatchase4, s_fatdeathcam, s_fatdie1,
    s_fatdie3, s_fatdie4, s_fatdie5, s_fatdie6, s_fatshoot1, s_fatshoot2,
    s_fatshoot3, s_fatshoot4, s_fatshoot5, s_fatshoot6;
static const statetype s_fakestand, s_fakechase1, s_fakechase1s,
    s_fakechase2, s_fakechase3, s_fakechase3s, s_fakechase4, s_fakedie1,
    s_fakedie2, s_fakedie3, s_fakedie4, s_fakedie5, s_fakedie6, s_fakeshoot1,
    s_fakeshoot2, s_fakeshoot3, s_fakeshoot4, s_fakeshoot5, s_fakeshoot6,
    s_fakeshoot7, s_fakeshoot8, s_fakeshoot9, s_fire1, s_fire2;
static const statetype s_mechastand, s_mechachase1, s_mechachase1s,
    s_mechachase2, s_mechachase3, s_mechachase3s, s_mechachase4, s_mechadie1,
    s_mechadie2, s_mechadie3, s_mechadie4, s_mechashoot1, s_mechashoot2,
    s_mechashoot3, s_mechashoot4, s_mechashoot5, s_mechashoot6;
static const statetype s_hitlerchase1, s_hitlerchase1s, s_hitlerchase2,
    s_hitlerchase3, s_hitlerchase3s, s_hitlerchase4, s_hitlerdeathcam,
    s_hitlerdie1, s_hitlerdie3, s_hitlerdie4, s_hitlerdie5, s_hitlerdie6,
    s_hitlerdie7, s_hitlerdie8, s_hitlerdie9, s_hitlerdie10, s_hitlershoot1,
    s_hitlershoot2, s_hitlershoot3, s_hitlershoot4, s_hitlershoot5,
    s_hitlershoot6;

/* Spear of Destiny's */
static const statetype s_hrocket, s_hsmoke1, s_hsmoke2, s_hsmoke3,
    s_hsmoke4, s_hboom1, s_hboom2, s_hboom3;
static const statetype s_transstand, s_transchase1, s_transchase1s,
    s_transchase2, s_transchase3, s_transchase3s, s_transchase4,
    s_transdie0, s_transdie1, s_transdie2, s_transdie3, s_transdie4,
    s_transshoot1, s_transshoot2, s_transshoot3, s_transshoot4,
    s_transshoot5, s_transshoot6, s_transshoot7, s_transshoot8;
static const statetype s_uberstand, s_uberchase1, s_uberchase1s,
    s_uberchase2, s_uberchase3, s_uberchase3s, s_uberchase4, s_uberdie0,
    s_uberdie1, s_uberdie2, s_uberdie3, s_uberdie4, s_uberdie5,
    s_ubershoot1, s_ubershoot2, s_ubershoot3, s_ubershoot4, s_ubershoot5,
    s_ubershoot6, s_ubershoot7;
static const statetype s_willstand, s_willchase1, s_willchase1s,
    s_willchase2, s_willchase3, s_willchase3s, s_willchase4, s_willdeathcam,
    s_willdie1, s_willdie3, s_willdie4, s_willdie5, s_willdie6,
    s_willshoot1, s_willshoot2, s_willshoot3, s_willshoot4, s_willshoot5,
    s_willshoot6;
static const statetype s_deathstand, s_deathchase1, s_deathchase1s,
    s_deathchase2, s_deathchase3, s_deathchase3s, s_deathchase4,
    s_deathdeathcam, s_deathdie1, s_deathdie3, s_deathdie4, s_deathdie5,
    s_deathdie6, s_deathdie7, s_deathdie8, s_deathdie9, s_deathshoot1,
    s_deathshoot2, s_deathshoot3, s_deathshoot4, s_deathshoot5;
static const statetype s_angelstand, s_angelchase1, s_angelchase1s,
    s_angelchase2, s_angelchase3, s_angelchase3s, s_angelchase4, s_angeldie1,
    s_angeldie2, s_angeldie3, s_angeldie4, s_angeldie5, s_angeldie6,
    s_angeldie7, s_angeldie8, s_angeldie9, s_angelshoot1, s_angelshoot2,
    s_angelshoot3, s_angeltired, s_angeltired2, s_angeltired3, s_angeltired4,
    s_angeltired5, s_angeltired6, s_angeltired7, s_spark1, s_spark2,
    s_spark3, s_spark4;
static const statetype s_spectrewait1, s_spectrewait2, s_spectrewait3,
    s_spectrewait4, s_spectrechase1, s_spectrechase2, s_spectrechase3,
    s_spectrechase4, s_spectredie1, s_spectredie2, s_spectredie3,
    s_spectredie4, s_spectrewake;

/* The states whose length the spawn routines set: the pause for a boss's
 * last words, as long as the recording where there is one - which the DOS
 * code decided by poking the shared table. */
static statetype s_schabbdie2, s_giftdie2, s_fatdie2, s_hitlerdie2;
static statetype s_transdie01, s_uberdie01, s_willdie2, s_deathdie2,
    s_angeldie11;

/* guard */
static const statetype s_grdstand   = {1, SPR_GRD_S_1,    0,  T_Stand, NULL, &s_grdstand};
static const statetype s_grdpath1   = {1, SPR_GRD_W1_1,   20, T_Path,  NULL, &s_grdpath1s};
static const statetype s_grdpath1s  = {1, SPR_GRD_W1_1,   5,  NULL,    NULL, &s_grdpath2};
static const statetype s_grdpath2   = {1, SPR_GRD_W2_1,   15, T_Path,  NULL, &s_grdpath3};
static const statetype s_grdpath3   = {1, SPR_GRD_W3_1,   20, T_Path,  NULL, &s_grdpath3s};
static const statetype s_grdpath3s  = {1, SPR_GRD_W3_1,   5,  NULL,    NULL, &s_grdpath4};
static const statetype s_grdpath4   = {1, SPR_GRD_W4_1,   15, T_Path,  NULL, &s_grdpath1};
static const statetype s_grdpain    = {2, SPR_GRD_PAIN_1, 10, NULL,    NULL, &s_grdchase1};
static const statetype s_grdpain1   = {2, SPR_GRD_PAIN_2, 10, NULL,    NULL, &s_grdchase1};
static const statetype s_grdshoot1  = {0, SPR_GRD_SHOOT1, 20, NULL,    NULL,    &s_grdshoot2};
static const statetype s_grdshoot2  = {0, SPR_GRD_SHOOT2, 20, NULL,    T_Shoot, &s_grdshoot3};
static const statetype s_grdshoot3  = {0, SPR_GRD_SHOOT3, 20, NULL,    NULL,    &s_grdchase1};
static const statetype s_grdchase1  = {1, SPR_GRD_W1_1,   10, T_Chase, NULL, &s_grdchase1s};
static const statetype s_grdchase1s = {1, SPR_GRD_W1_1,   3,  NULL,    NULL, &s_grdchase2};
static const statetype s_grdchase2  = {1, SPR_GRD_W2_1,   8,  T_Chase, NULL, &s_grdchase3};
static const statetype s_grdchase3  = {1, SPR_GRD_W3_1,   10, T_Chase, NULL, &s_grdchase3s};
static const statetype s_grdchase3s = {1, SPR_GRD_W3_1,   3,  NULL,    NULL, &s_grdchase4};
static const statetype s_grdchase4  = {1, SPR_GRD_W4_1,   8,  T_Chase, NULL, &s_grdchase1};
static const statetype s_grddie1    = {0, SPR_GRD_DIE_1,  15, NULL, A_DeathScream, &s_grddie2};
static const statetype s_grddie2    = {0, SPR_GRD_DIE_2,  15, NULL, NULL, &s_grddie3};
static const statetype s_grddie3    = {0, SPR_GRD_DIE_3,  15, NULL, NULL, &s_grddie4};
static const statetype s_grddie4    = {0, SPR_GRD_DEAD,   0,  NULL, NULL, &s_grddie4};

/* dog */
static const statetype s_dogpath1   = {1, SPR_DOG_W1_1,  20, T_Path,     NULL, &s_dogpath1s};
static const statetype s_dogpath1s  = {1, SPR_DOG_W1_1,  5,  NULL,       NULL, &s_dogpath2};
static const statetype s_dogpath2   = {1, SPR_DOG_W2_1,  15, T_Path,     NULL, &s_dogpath3};
static const statetype s_dogpath3   = {1, SPR_DOG_W3_1,  20, T_Path,     NULL, &s_dogpath3s};
static const statetype s_dogpath3s  = {1, SPR_DOG_W3_1,  5,  NULL,       NULL, &s_dogpath4};
static const statetype s_dogpath4   = {1, SPR_DOG_W4_1,  15, T_Path,     NULL, &s_dogpath1};
static const statetype s_dogjump1   = {0, SPR_DOG_JUMP1, 10, NULL,       NULL,   &s_dogjump2};
static const statetype s_dogjump2   = {0, SPR_DOG_JUMP2, 10, NULL,       T_Bite, &s_dogjump3};
static const statetype s_dogjump3   = {0, SPR_DOG_JUMP3, 10, NULL,       NULL,   &s_dogjump4};
static const statetype s_dogjump4   = {0, SPR_DOG_JUMP1, 10, NULL,       NULL,   &s_dogjump5};
static const statetype s_dogjump5   = {0, SPR_DOG_W1_1,  10, NULL,       NULL,   &s_dogchase1};
static const statetype s_dogchase1  = {1, SPR_DOG_W1_1,  10, T_DogChase, NULL, &s_dogchase1s};
static const statetype s_dogchase1s = {1, SPR_DOG_W1_1,  3,  NULL,       NULL, &s_dogchase2};
static const statetype s_dogchase2  = {1, SPR_DOG_W2_1,  8,  T_DogChase, NULL, &s_dogchase3};
static const statetype s_dogchase3  = {1, SPR_DOG_W3_1,  10, T_DogChase, NULL, &s_dogchase3s};
static const statetype s_dogchase3s = {1, SPR_DOG_W3_1,  3,  NULL,       NULL, &s_dogchase4};
static const statetype s_dogchase4  = {1, SPR_DOG_W4_1,  8,  T_DogChase, NULL, &s_dogchase1};
static const statetype s_dogdie1    = {0, SPR_DOG_DIE_1, 15, NULL, A_DeathScream, &s_dogdie2};
static const statetype s_dogdie2    = {0, SPR_DOG_DIE_2, 15, NULL, NULL, &s_dogdie3};
static const statetype s_dogdie3    = {0, SPR_DOG_DIE_3, 15, NULL, NULL, &s_dogdead};
static const statetype s_dogdead    = {0, SPR_DOG_DEAD,  15, NULL, NULL, &s_dogdead};

/* SS */
static const statetype s_ssstand    = {1, SPR_SS_S_1,    0,  T_Stand, NULL, &s_ssstand};
static const statetype s_sspath1    = {1, SPR_SS_W1_1,   20, T_Path,  NULL, &s_sspath1s};
static const statetype s_sspath1s   = {1, SPR_SS_W1_1,   5,  NULL,    NULL, &s_sspath2};
static const statetype s_sspath2    = {1, SPR_SS_W2_1,   15, T_Path,  NULL, &s_sspath3};
static const statetype s_sspath3    = {1, SPR_SS_W3_1,   20, T_Path,  NULL, &s_sspath3s};
static const statetype s_sspath3s   = {1, SPR_SS_W3_1,   5,  NULL,    NULL, &s_sspath4};
static const statetype s_sspath4    = {1, SPR_SS_W4_1,   15, T_Path,  NULL, &s_sspath1};
static const statetype s_sspain     = {2, SPR_SS_PAIN_1, 10, NULL,    NULL, &s_sschase1};
static const statetype s_sspain1    = {2, SPR_SS_PAIN_2, 10, NULL,    NULL, &s_sschase1};
static const statetype s_ssshoot1   = {0, SPR_SS_SHOOT1, 20, NULL, NULL,    &s_ssshoot2};
static const statetype s_ssshoot2   = {0, SPR_SS_SHOOT2, 20, NULL, T_Shoot, &s_ssshoot3};
static const statetype s_ssshoot3   = {0, SPR_SS_SHOOT3, 10, NULL, NULL,    &s_ssshoot4};
static const statetype s_ssshoot4   = {0, SPR_SS_SHOOT2, 10, NULL, T_Shoot, &s_ssshoot5};
static const statetype s_ssshoot5   = {0, SPR_SS_SHOOT3, 10, NULL, NULL,    &s_ssshoot6};
static const statetype s_ssshoot6   = {0, SPR_SS_SHOOT2, 10, NULL, T_Shoot, &s_ssshoot7};
static const statetype s_ssshoot7   = {0, SPR_SS_SHOOT3, 10, NULL, NULL,    &s_ssshoot8};
static const statetype s_ssshoot8   = {0, SPR_SS_SHOOT2, 10, NULL, T_Shoot, &s_ssshoot9};
static const statetype s_ssshoot9   = {0, SPR_SS_SHOOT3, 10, NULL, NULL,    &s_sschase1};
static const statetype s_sschase1   = {1, SPR_SS_W1_1,   10, T_Chase, NULL, &s_sschase1s};
static const statetype s_sschase1s  = {1, SPR_SS_W1_1,   3,  NULL,    NULL, &s_sschase2};
static const statetype s_sschase2   = {1, SPR_SS_W2_1,   8,  T_Chase, NULL, &s_sschase3};
static const statetype s_sschase3   = {1, SPR_SS_W3_1,   10, T_Chase, NULL, &s_sschase3s};
static const statetype s_sschase3s  = {1, SPR_SS_W3_1,   3,  NULL,    NULL, &s_sschase4};
static const statetype s_sschase4   = {1, SPR_SS_W4_1,   8,  T_Chase, NULL, &s_sschase1};
static const statetype s_ssdie1     = {0, SPR_SS_DIE_1,  15, NULL, A_DeathScream, &s_ssdie2};
static const statetype s_ssdie2     = {0, SPR_SS_DIE_2,  15, NULL, NULL, &s_ssdie3};
static const statetype s_ssdie3     = {0, SPR_SS_DIE_3,  15, NULL, NULL, &s_ssdie4};
static const statetype s_ssdie4     = {0, SPR_SS_DEAD,   0,  NULL, NULL, &s_ssdie4};

/* Hans Grosse */
static const statetype s_bossstand   = {0, SPR_BOSS_W1,     0,  T_Stand, NULL, &s_bossstand};
static const statetype s_bosschase1  = {0, SPR_BOSS_W1,     10, T_Chase, NULL, &s_bosschase1s};
static const statetype s_bosschase1s = {0, SPR_BOSS_W1,     3,  NULL,    NULL, &s_bosschase2};
static const statetype s_bosschase2  = {0, SPR_BOSS_W2,     8,  T_Chase, NULL, &s_bosschase3};
static const statetype s_bosschase3  = {0, SPR_BOSS_W3,     10, T_Chase, NULL, &s_bosschase3s};
static const statetype s_bosschase3s = {0, SPR_BOSS_W3,     3,  NULL,    NULL, &s_bosschase4};
static const statetype s_bosschase4  = {0, SPR_BOSS_W4,     8,  T_Chase, NULL, &s_bosschase1};
static const statetype s_bossdie1    = {0, SPR_BOSS_DIE1,   15, NULL, A_DeathScream, &s_bossdie2};
static const statetype s_bossdie2    = {0, SPR_BOSS_DIE2,   15, NULL, NULL, &s_bossdie3};
static const statetype s_bossdie3    = {0, SPR_BOSS_DIE3,   15, NULL, NULL, &s_bossdie4};
static const statetype s_bossdie4    = {0, SPR_BOSS_DEAD,   0,  NULL, NULL, &s_bossdie4};
static const statetype s_bossshoot1  = {0, SPR_BOSS_SHOOT1, 30, NULL, NULL,    &s_bossshoot2};
static const statetype s_bossshoot2  = {0, SPR_BOSS_SHOOT2, 10, NULL, T_Shoot, &s_bossshoot3};
static const statetype s_bossshoot3  = {0, SPR_BOSS_SHOOT3, 10, NULL, T_Shoot, &s_bossshoot4};
static const statetype s_bossshoot4  = {0, SPR_BOSS_SHOOT2, 10, NULL, T_Shoot, &s_bossshoot5};
static const statetype s_bossshoot5  = {0, SPR_BOSS_SHOOT3, 10, NULL, T_Shoot, &s_bossshoot6};
static const statetype s_bossshoot6  = {0, SPR_BOSS_SHOOT2, 10, NULL, T_Shoot, &s_bossshoot7};
static const statetype s_bossshoot7  = {0, SPR_BOSS_SHOOT3, 10, NULL, T_Shoot, &s_bossshoot8};
static const statetype s_bossshoot8  = {0, SPR_BOSS_SHOOT1, 10, NULL, NULL,    &s_bosschase1};

/* BJ, running out of the castle */
static const statetype s_bjrun1  = {0, SPR_BJ_W1,    12,  T_BJRun,  NULL,     &s_bjrun1s};
static const statetype s_bjrun1s = {0, SPR_BJ_W1,    3,   NULL,     NULL,     &s_bjrun2};
static const statetype s_bjrun2  = {0, SPR_BJ_W2,    8,   T_BJRun,  NULL,     &s_bjrun3};
static const statetype s_bjrun3  = {0, SPR_BJ_W3,    12,  T_BJRun,  NULL,     &s_bjrun3s};
static const statetype s_bjrun3s = {0, SPR_BJ_W3,    3,   NULL,     NULL,     &s_bjrun4};
static const statetype s_bjrun4  = {0, SPR_BJ_W4,    8,   T_BJRun,  NULL,     &s_bjrun1};
static const statetype s_bjjump1 = {0, SPR_BJ_JUMP1, 14,  T_BJJump, NULL,     &s_bjjump2};
static const statetype s_bjjump2 = {0, SPR_BJ_JUMP2, 14,  T_BJJump, T_BJYell, &s_bjjump3};
static const statetype s_bjjump3 = {0, SPR_BJ_JUMP3, 14,  T_BJJump, NULL,     &s_bjjump4};
static const statetype s_bjjump4 = {0, SPR_BJ_JUMP4, 300, NULL,     T_BJDone, &s_bjjump4};

/* rockets, and their smoke and explosions; a state with no next is the end
 * of the actor */
static const statetype s_rocket  = {1, SPR_ROCKET_1, 3, T_Projectile, A_Smoke, &s_rocket};
static const statetype s_smoke1  = {0, SPR_SMOKE_1,  3, NULL, NULL, &s_smoke2};
static const statetype s_smoke2  = {0, SPR_SMOKE_2,  3, NULL, NULL, &s_smoke3};
static const statetype s_smoke3  = {0, SPR_SMOKE_3,  3, NULL, NULL, &s_smoke4};
static const statetype s_smoke4  = {0, SPR_SMOKE_4,  3, NULL, NULL, NULL};
static const statetype s_boom1   = {0, SPR_BOOM_1,   6, NULL, NULL, &s_boom2};
static const statetype s_boom2   = {0, SPR_BOOM_2,   6, NULL, NULL, &s_boom3};
static const statetype s_boom3   = {0, SPR_BOOM_3,   6, NULL, NULL, NULL};

/* officer */
static const statetype s_ofcstand   = {1, SPR_OFC_S_1,    0,  T_Stand, NULL, &s_ofcstand};
static const statetype s_ofcpath1   = {1, SPR_OFC_W1_1,   20, T_Path,  NULL, &s_ofcpath1s};
static const statetype s_ofcpath1s  = {1, SPR_OFC_W1_1,   5,  NULL,    NULL, &s_ofcpath2};
static const statetype s_ofcpath2   = {1, SPR_OFC_W2_1,   15, T_Path,  NULL, &s_ofcpath3};
static const statetype s_ofcpath3   = {1, SPR_OFC_W3_1,   20, T_Path,  NULL, &s_ofcpath3s};
static const statetype s_ofcpath3s  = {1, SPR_OFC_W3_1,   5,  NULL,    NULL, &s_ofcpath4};
static const statetype s_ofcpath4   = {1, SPR_OFC_W4_1,   15, T_Path,  NULL, &s_ofcpath1};
static const statetype s_ofcpain    = {2, SPR_OFC_PAIN_1, 10, NULL,    NULL, &s_ofcchase1};
static const statetype s_ofcpain1   = {2, SPR_OFC_PAIN_2, 10, NULL,    NULL, &s_ofcchase1};
static const statetype s_ofcshoot1  = {0, SPR_OFC_SHOOT1, 6,  NULL,    NULL,    &s_ofcshoot2};
static const statetype s_ofcshoot2  = {0, SPR_OFC_SHOOT2, 20, NULL,    T_Shoot, &s_ofcshoot3};
static const statetype s_ofcshoot3  = {0, SPR_OFC_SHOOT3, 10, NULL,    NULL,    &s_ofcchase1};
static const statetype s_ofcchase1  = {1, SPR_OFC_W1_1,   10, T_Chase, NULL, &s_ofcchase1s};
static const statetype s_ofcchase1s = {1, SPR_OFC_W1_1,   3,  NULL,    NULL, &s_ofcchase2};
static const statetype s_ofcchase2  = {1, SPR_OFC_W2_1,   8,  T_Chase, NULL, &s_ofcchase3};
static const statetype s_ofcchase3  = {1, SPR_OFC_W3_1,   10, T_Chase, NULL, &s_ofcchase3s};
static const statetype s_ofcchase3s = {1, SPR_OFC_W3_1,   3,  NULL,    NULL, &s_ofcchase4};
static const statetype s_ofcchase4  = {1, SPR_OFC_W4_1,   8,  T_Chase, NULL, &s_ofcchase1};
static const statetype s_ofcdie1    = {0, SPR_OFC_DIE_1,  11, NULL, A_DeathScream, &s_ofcdie2};
static const statetype s_ofcdie2    = {0, SPR_OFC_DIE_2,  11, NULL, NULL, &s_ofcdie3};
static const statetype s_ofcdie3    = {0, SPR_OFC_DIE_3,  11, NULL, NULL, &s_ofcdie4};
static const statetype s_ofcdie4    = {0, SPR_OFC_DIE_4,  11, NULL, NULL, &s_ofcdie5};
static const statetype s_ofcdie5    = {0, SPR_OFC_DEAD,   0,  NULL, NULL, &s_ofcdie5};

/* mutant */
static const statetype s_mutstand   = {1, SPR_MUT_S_1,    0,  T_Stand, NULL, &s_mutstand};
static const statetype s_mutpath1   = {1, SPR_MUT_W1_1,   20, T_Path,  NULL, &s_mutpath1s};
static const statetype s_mutpath1s  = {1, SPR_MUT_W1_1,   5,  NULL,    NULL, &s_mutpath2};
static const statetype s_mutpath2   = {1, SPR_MUT_W2_1,   15, T_Path,  NULL, &s_mutpath3};
static const statetype s_mutpath3   = {1, SPR_MUT_W3_1,   20, T_Path,  NULL, &s_mutpath3s};
static const statetype s_mutpath3s  = {1, SPR_MUT_W3_1,   5,  NULL,    NULL, &s_mutpath4};
static const statetype s_mutpath4   = {1, SPR_MUT_W4_1,   15, T_Path,  NULL, &s_mutpath1};
static const statetype s_mutpain    = {2, SPR_MUT_PAIN_1, 10, NULL,    NULL, &s_mutchase1};
static const statetype s_mutpain1   = {2, SPR_MUT_PAIN_2, 10, NULL,    NULL, &s_mutchase1};
static const statetype s_mutshoot1  = {0, SPR_MUT_SHOOT1, 6,  NULL,    T_Shoot, &s_mutshoot2};
static const statetype s_mutshoot2  = {0, SPR_MUT_SHOOT2, 20, NULL,    NULL,    &s_mutshoot3};
static const statetype s_mutshoot3  = {0, SPR_MUT_SHOOT3, 10, NULL,    T_Shoot, &s_mutshoot4};
static const statetype s_mutshoot4  = {0, SPR_MUT_SHOOT4, 20, NULL,    NULL,    &s_mutchase1};
static const statetype s_mutchase1  = {1, SPR_MUT_W1_1,   10, T_Chase, NULL, &s_mutchase1s};
static const statetype s_mutchase1s = {1, SPR_MUT_W1_1,   3,  NULL,    NULL, &s_mutchase2};
static const statetype s_mutchase2  = {1, SPR_MUT_W2_1,   8,  T_Chase, NULL, &s_mutchase3};
static const statetype s_mutchase3  = {1, SPR_MUT_W3_1,   10, T_Chase, NULL, &s_mutchase3s};
static const statetype s_mutchase3s = {1, SPR_MUT_W3_1,   3,  NULL,    NULL, &s_mutchase4};
static const statetype s_mutchase4  = {1, SPR_MUT_W4_1,   8,  T_Chase, NULL, &s_mutchase1};
static const statetype s_mutdie1    = {0, SPR_MUT_DIE_1,  7,  NULL, A_DeathScream, &s_mutdie2};
static const statetype s_mutdie2    = {0, SPR_MUT_DIE_2,  7,  NULL, NULL, &s_mutdie3};
static const statetype s_mutdie3    = {0, SPR_MUT_DIE_3,  7,  NULL, NULL, &s_mutdie4};
static const statetype s_mutdie4    = {0, SPR_MUT_DIE_4,  7,  NULL, NULL, &s_mutdie5};
static const statetype s_mutdie5    = {0, SPR_MUT_DEAD,   0,  NULL, NULL, &s_mutdie5};

/* the Pac-Man ghosts of the secret floor in episode three */
static const statetype s_blinkychase1 = {0, SPR_BLINKY_W1, 10, T_Ghosts, NULL, &s_blinkychase2};
static const statetype s_blinkychase2 = {0, SPR_BLINKY_W2, 10, T_Ghosts, NULL, &s_blinkychase1};
static const statetype s_inkychase1   = {0, SPR_INKY_W1,   10, T_Ghosts, NULL, &s_inkychase2};
static const statetype s_inkychase2   = {0, SPR_INKY_W2,   10, T_Ghosts, NULL, &s_inkychase1};
static const statetype s_pinkychase1  = {0, SPR_PINKY_W1,  10, T_Ghosts, NULL, &s_pinkychase2};
static const statetype s_pinkychase2  = {0, SPR_PINKY_W2,  10, T_Ghosts, NULL, &s_pinkychase1};
static const statetype s_clydechase1  = {0, SPR_CLYDE_W1,  10, T_Ghosts, NULL, &s_clydechase2};
static const statetype s_clydechase2  = {0, SPR_CLYDE_W2,  10, T_Ghosts, NULL, &s_clydechase1};

/* Gretel Grosse */
static const statetype s_gretelstand   = {0, SPR_GRETEL_W1,     0,  T_Stand, NULL, &s_gretelstand};
static const statetype s_gretelchase1  = {0, SPR_GRETEL_W1,     10, T_Chase, NULL, &s_gretelchase1s};
static const statetype s_gretelchase1s = {0, SPR_GRETEL_W1,     3,  NULL,    NULL, &s_gretelchase2};
static const statetype s_gretelchase2  = {0, SPR_GRETEL_W2,     8,  T_Chase, NULL, &s_gretelchase3};
static const statetype s_gretelchase3  = {0, SPR_GRETEL_W3,     10, T_Chase, NULL, &s_gretelchase3s};
static const statetype s_gretelchase3s = {0, SPR_GRETEL_W3,     3,  NULL,    NULL, &s_gretelchase4};
static const statetype s_gretelchase4  = {0, SPR_GRETEL_W4,     8,  T_Chase, NULL, &s_gretelchase1};
static const statetype s_greteldie1    = {0, SPR_GRETEL_DIE1,   15, NULL, A_DeathScream, &s_greteldie2};
static const statetype s_greteldie2    = {0, SPR_GRETEL_DIE2,   15, NULL, NULL, &s_greteldie3};
static const statetype s_greteldie3    = {0, SPR_GRETEL_DIE3,   15, NULL, NULL, &s_greteldie4};
static const statetype s_greteldie4    = {0, SPR_GRETEL_DEAD,   0,  NULL, NULL, &s_greteldie4};
static const statetype s_gretelshoot1  = {0, SPR_GRETEL_SHOOT1, 30, NULL, NULL,    &s_gretelshoot2};
static const statetype s_gretelshoot2  = {0, SPR_GRETEL_SHOOT2, 10, NULL, T_Shoot, &s_gretelshoot3};
static const statetype s_gretelshoot3  = {0, SPR_GRETEL_SHOOT3, 10, NULL, T_Shoot, &s_gretelshoot4};
static const statetype s_gretelshoot4  = {0, SPR_GRETEL_SHOOT2, 10, NULL, T_Shoot, &s_gretelshoot5};
static const statetype s_gretelshoot5  = {0, SPR_GRETEL_SHOOT3, 10, NULL, T_Shoot, &s_gretelshoot6};
static const statetype s_gretelshoot6  = {0, SPR_GRETEL_SHOOT2, 10, NULL, T_Shoot, &s_gretelshoot7};
static const statetype s_gretelshoot7  = {0, SPR_GRETEL_SHOOT3, 10, NULL, T_Shoot, &s_gretelshoot8};
static const statetype s_gretelshoot8  = {0, SPR_GRETEL_SHOOT1, 10, NULL, NULL,    &s_gretelchase1};

/* Dr. Schabbs, and his syringes */
static const statetype s_schabbstand   = {0, SPR_SCHABB_W1, 0,  T_Stand,  NULL, &s_schabbstand};
static const statetype s_schabbchase1  = {0, SPR_SCHABB_W1, 10, T_Schabb, NULL, &s_schabbchase1s};
static const statetype s_schabbchase1s = {0, SPR_SCHABB_W1, 3,  NULL,     NULL, &s_schabbchase2};
static const statetype s_schabbchase2  = {0, SPR_SCHABB_W2, 8,  T_Schabb, NULL, &s_schabbchase3};
static const statetype s_schabbchase3  = {0, SPR_SCHABB_W3, 10, T_Schabb, NULL, &s_schabbchase3s};
static const statetype s_schabbchase3s = {0, SPR_SCHABB_W3, 3,  NULL,     NULL, &s_schabbchase4};
static const statetype s_schabbchase4  = {0, SPR_SCHABB_W4, 8,  T_Schabb, NULL, &s_schabbchase1};
static const statetype s_schabbdeathcam = {0, SPR_SCHABB_W1, 1, NULL, NULL, &s_schabbdie1};
static const statetype s_schabbdie1    = {0, SPR_SCHABB_W1,   10, NULL, A_DeathScream, &s_schabbdie2};
static statetype       s_schabbdie2    = {0, SPR_SCHABB_W1,   10, NULL, NULL, &s_schabbdie3};
static const statetype s_schabbdie3    = {0, SPR_SCHABB_DIE1, 10, NULL, NULL, &s_schabbdie4};
static const statetype s_schabbdie4    = {0, SPR_SCHABB_DIE2, 10, NULL, NULL, &s_schabbdie5};
static const statetype s_schabbdie5    = {0, SPR_SCHABB_DIE3, 10, NULL, NULL, &s_schabbdie6};
static const statetype s_schabbdie6    = {0, SPR_SCHABB_DEAD, 20, NULL, A_StartDeathCam, &s_schabbdie6};
static const statetype s_schabbshoot1  = {0, SPR_SCHABB_SHOOT1, 30, NULL, NULL, &s_schabbshoot2};
static const statetype s_schabbshoot2  = {0, SPR_SCHABB_SHOOT2, 10, NULL, T_SchabbThrow, &s_schabbchase1};
static const statetype s_needle1       = {0, SPR_HYPO1, 6, T_Projectile, NULL, &s_needle2};
static const statetype s_needle2       = {0, SPR_HYPO2, 6, T_Projectile, NULL, &s_needle3};
static const statetype s_needle3       = {0, SPR_HYPO3, 6, T_Projectile, NULL, &s_needle4};
static const statetype s_needle4       = {0, SPR_HYPO4, 6, T_Projectile, NULL, &s_needle1};

/* Otto Giftmacher */
static const statetype s_giftstand   = {0, SPR_GIFT_W1, 0,  T_Stand, NULL, &s_giftstand};
static const statetype s_giftchase1  = {0, SPR_GIFT_W1, 10, T_Gift,  NULL, &s_giftchase1s};
static const statetype s_giftchase1s = {0, SPR_GIFT_W1, 3,  NULL,    NULL, &s_giftchase2};
static const statetype s_giftchase2  = {0, SPR_GIFT_W2, 8,  T_Gift,  NULL, &s_giftchase3};
static const statetype s_giftchase3  = {0, SPR_GIFT_W3, 10, T_Gift,  NULL, &s_giftchase3s};
static const statetype s_giftchase3s = {0, SPR_GIFT_W3, 3,  NULL,    NULL, &s_giftchase4};
static const statetype s_giftchase4  = {0, SPR_GIFT_W4, 8,  T_Gift,  NULL, &s_giftchase1};
static const statetype s_giftdeathcam = {0, SPR_GIFT_W1, 1, NULL, NULL, &s_giftdie1};
static const statetype s_giftdie1    = {0, SPR_GIFT_W1,   1,  NULL, A_DeathScream, &s_giftdie2};
static statetype       s_giftdie2    = {0, SPR_GIFT_W1,   10, NULL, NULL, &s_giftdie3};
static const statetype s_giftdie3    = {0, SPR_GIFT_DIE1, 10, NULL, NULL, &s_giftdie4};
static const statetype s_giftdie4    = {0, SPR_GIFT_DIE2, 10, NULL, NULL, &s_giftdie5};
static const statetype s_giftdie5    = {0, SPR_GIFT_DIE3, 10, NULL, NULL, &s_giftdie6};
static const statetype s_giftdie6    = {0, SPR_GIFT_DEAD, 20, NULL, A_StartDeathCam, &s_giftdie6};
static const statetype s_giftshoot1  = {0, SPR_GIFT_SHOOT1, 30, NULL, NULL, &s_giftshoot2};
static const statetype s_giftshoot2  = {0, SPR_GIFT_SHOOT2, 10, NULL, T_GiftThrow, &s_giftchase1};

/* General Fettgesicht */
static const statetype s_fatstand   = {0, SPR_FAT_W1, 0,  T_Stand, NULL, &s_fatstand};
static const statetype s_fatchase1  = {0, SPR_FAT_W1, 10, T_Fat,   NULL, &s_fatchase1s};
static const statetype s_fatchase1s = {0, SPR_FAT_W1, 3,  NULL,    NULL, &s_fatchase2};
static const statetype s_fatchase2  = {0, SPR_FAT_W2, 8,  T_Fat,   NULL, &s_fatchase3};
static const statetype s_fatchase3  = {0, SPR_FAT_W3, 10, T_Fat,   NULL, &s_fatchase3s};
static const statetype s_fatchase3s = {0, SPR_FAT_W3, 3,  NULL,    NULL, &s_fatchase4};
static const statetype s_fatchase4  = {0, SPR_FAT_W4, 8,  T_Fat,   NULL, &s_fatchase1};
static const statetype s_fatdeathcam = {0, SPR_FAT_W1, 1, NULL, NULL, &s_fatdie1};
static const statetype s_fatdie1    = {0, SPR_FAT_W1,   1,  NULL, A_DeathScream, &s_fatdie2};
static statetype       s_fatdie2    = {0, SPR_FAT_W1,   10, NULL, NULL, &s_fatdie3};
static const statetype s_fatdie3    = {0, SPR_FAT_DIE1, 10, NULL, NULL, &s_fatdie4};
static const statetype s_fatdie4    = {0, SPR_FAT_DIE2, 10, NULL, NULL, &s_fatdie5};
static const statetype s_fatdie5    = {0, SPR_FAT_DIE3, 10, NULL, NULL, &s_fatdie6};
static const statetype s_fatdie6    = {0, SPR_FAT_DEAD, 20, NULL, A_StartDeathCam, &s_fatdie6};
static const statetype s_fatshoot1  = {0, SPR_FAT_SHOOT1, 30, NULL, NULL,        &s_fatshoot2};
static const statetype s_fatshoot2  = {0, SPR_FAT_SHOOT2, 10, NULL, T_GiftThrow, &s_fatshoot3};
static const statetype s_fatshoot3  = {0, SPR_FAT_SHOOT3, 10, NULL, T_Shoot,     &s_fatshoot4};
static const statetype s_fatshoot4  = {0, SPR_FAT_SHOOT4, 10, NULL, T_Shoot,     &s_fatshoot5};
static const statetype s_fatshoot5  = {0, SPR_FAT_SHOOT3, 10, NULL, T_Shoot,     &s_fatshoot6};
static const statetype s_fatshoot6  = {0, SPR_FAT_SHOOT4, 10, NULL, T_Shoot,     &s_fatchase1};

/* the fake Hitler, and his flames */
static const statetype s_fakestand   = {0, SPR_FAKE_W1, 0,  T_Stand, NULL, &s_fakestand};
static const statetype s_fakechase1  = {0, SPR_FAKE_W1, 10, T_Fake,  NULL, &s_fakechase1s};
static const statetype s_fakechase1s = {0, SPR_FAKE_W1, 3,  NULL,    NULL, &s_fakechase2};
static const statetype s_fakechase2  = {0, SPR_FAKE_W2, 8,  T_Fake,  NULL, &s_fakechase3};
static const statetype s_fakechase3  = {0, SPR_FAKE_W3, 10, T_Fake,  NULL, &s_fakechase3s};
static const statetype s_fakechase3s = {0, SPR_FAKE_W3, 3,  NULL,    NULL, &s_fakechase4};
static const statetype s_fakechase4  = {0, SPR_FAKE_W4, 8,  T_Fake,  NULL, &s_fakechase1};
static const statetype s_fakedie1    = {0, SPR_FAKE_DIE1, 10, NULL, A_DeathScream, &s_fakedie2};
static const statetype s_fakedie2    = {0, SPR_FAKE_DIE2, 10, NULL, NULL, &s_fakedie3};
static const statetype s_fakedie3    = {0, SPR_FAKE_DIE3, 10, NULL, NULL, &s_fakedie4};
static const statetype s_fakedie4    = {0, SPR_FAKE_DIE4, 10, NULL, NULL, &s_fakedie5};
static const statetype s_fakedie5    = {0, SPR_FAKE_DIE5, 10, NULL, NULL, &s_fakedie6};
static const statetype s_fakedie6    = {0, SPR_FAKE_DEAD, 0,  NULL, NULL, &s_fakedie6};
static const statetype s_fakeshoot1  = {0, SPR_FAKE_SHOOT, 8, NULL, T_FakeFire, &s_fakeshoot2};
static const statetype s_fakeshoot2  = {0, SPR_FAKE_SHOOT, 8, NULL, T_FakeFire, &s_fakeshoot3};
static const statetype s_fakeshoot3  = {0, SPR_FAKE_SHOOT, 8, NULL, T_FakeFire, &s_fakeshoot4};
static const statetype s_fakeshoot4  = {0, SPR_FAKE_SHOOT, 8, NULL, T_FakeFire, &s_fakeshoot5};
static const statetype s_fakeshoot5  = {0, SPR_FAKE_SHOOT, 8, NULL, T_FakeFire, &s_fakeshoot6};
static const statetype s_fakeshoot6  = {0, SPR_FAKE_SHOOT, 8, NULL, T_FakeFire, &s_fakeshoot7};
static const statetype s_fakeshoot7  = {0, SPR_FAKE_SHOOT, 8, NULL, T_FakeFire, &s_fakeshoot8};
static const statetype s_fakeshoot8  = {0, SPR_FAKE_SHOOT, 8, NULL, T_FakeFire, &s_fakeshoot9};
static const statetype s_fakeshoot9  = {0, SPR_FAKE_SHOOT, 8, NULL, NULL,       &s_fakechase1};
static const statetype s_fire1       = {0, SPR_FIRE1, 6, NULL, T_Projectile, &s_fire2};
static const statetype s_fire2       = {0, SPR_FIRE2, 6, NULL, T_Projectile, &s_fire1};

/* Hitler, in the mech suit and out of it */
static const statetype s_mechastand   = {0, SPR_MECHA_W1, 0,  T_Stand, NULL,         &s_mechastand};
static const statetype s_mechachase1  = {0, SPR_MECHA_W1, 10, T_Chase, A_MechaSound, &s_mechachase1s};
static const statetype s_mechachase1s = {0, SPR_MECHA_W1, 6,  NULL,    NULL,         &s_mechachase2};
static const statetype s_mechachase2  = {0, SPR_MECHA_W2, 8,  T_Chase, NULL,         &s_mechachase3};
static const statetype s_mechachase3  = {0, SPR_MECHA_W3, 10, T_Chase, A_MechaSound, &s_mechachase3s};
static const statetype s_mechachase3s = {0, SPR_MECHA_W3, 6,  NULL,    NULL,         &s_mechachase4};
static const statetype s_mechachase4  = {0, SPR_MECHA_W4, 8,  T_Chase, NULL,         &s_mechachase1};
static const statetype s_mechadie1    = {0, SPR_MECHA_DIE1, 10, NULL, A_DeathScream, &s_mechadie2};
static const statetype s_mechadie2    = {0, SPR_MECHA_DIE2, 10, NULL, NULL,          &s_mechadie3};
static const statetype s_mechadie3    = {0, SPR_MECHA_DIE3, 10, NULL, A_HitlerMorph, &s_mechadie4};
static const statetype s_mechadie4    = {0, SPR_MECHA_DEAD, 0,  NULL, NULL,          &s_mechadie4};
static const statetype s_mechashoot1  = {0, SPR_MECHA_SHOOT1, 30, NULL, NULL,    &s_mechashoot2};
static const statetype s_mechashoot2  = {0, SPR_MECHA_SHOOT2, 10, NULL, T_Shoot, &s_mechashoot3};
static const statetype s_mechashoot3  = {0, SPR_MECHA_SHOOT3, 10, NULL, T_Shoot, &s_mechashoot4};
static const statetype s_mechashoot4  = {0, SPR_MECHA_SHOOT2, 10, NULL, T_Shoot, &s_mechashoot5};
static const statetype s_mechashoot5  = {0, SPR_MECHA_SHOOT3, 10, NULL, T_Shoot, &s_mechashoot6};
static const statetype s_mechashoot6  = {0, SPR_MECHA_SHOOT2, 10, NULL, T_Shoot, &s_mechachase1};
static const statetype s_hitlerchase1  = {0, SPR_HITLER_W1, 6, T_Chase, NULL, &s_hitlerchase1s};
static const statetype s_hitlerchase1s = {0, SPR_HITLER_W1, 4, NULL,    NULL, &s_hitlerchase2};
static const statetype s_hitlerchase2  = {0, SPR_HITLER_W2, 2, T_Chase, NULL, &s_hitlerchase3};
static const statetype s_hitlerchase3  = {0, SPR_HITLER_W3, 6, T_Chase, NULL, &s_hitlerchase3s};
static const statetype s_hitlerchase3s = {0, SPR_HITLER_W3, 4, NULL,    NULL, &s_hitlerchase4};
static const statetype s_hitlerchase4  = {0, SPR_HITLER_W4, 2, T_Chase, NULL, &s_hitlerchase1};
static const statetype s_hitlerdeathcam = {0, SPR_HITLER_W1, 10, NULL, NULL, &s_hitlerdie1};
static const statetype s_hitlerdie1   = {0, SPR_HITLER_W1,   1,  NULL, A_DeathScream, &s_hitlerdie2};
static statetype       s_hitlerdie2   = {0, SPR_HITLER_W1,   10, NULL, NULL, &s_hitlerdie3};
static const statetype s_hitlerdie3   = {0, SPR_HITLER_DIE1, 10, NULL, A_Slurpie, &s_hitlerdie4};
static const statetype s_hitlerdie4   = {0, SPR_HITLER_DIE2, 10, NULL, NULL, &s_hitlerdie5};
static const statetype s_hitlerdie5   = {0, SPR_HITLER_DIE3, 10, NULL, NULL, &s_hitlerdie6};
static const statetype s_hitlerdie6   = {0, SPR_HITLER_DIE4, 10, NULL, NULL, &s_hitlerdie7};
static const statetype s_hitlerdie7   = {0, SPR_HITLER_DIE5, 10, NULL, NULL, &s_hitlerdie8};
static const statetype s_hitlerdie8   = {0, SPR_HITLER_DIE6, 10, NULL, NULL, &s_hitlerdie9};
static const statetype s_hitlerdie9   = {0, SPR_HITLER_DIE7, 10, NULL, NULL, &s_hitlerdie10};
static const statetype s_hitlerdie10  = {0, SPR_HITLER_DEAD, 20, NULL, A_StartDeathCam, &s_hitlerdie10};
static const statetype s_hitlershoot1 = {0, SPR_HITLER_SHOOT1, 30, NULL, NULL,    &s_hitlershoot2};
static const statetype s_hitlershoot2 = {0, SPR_HITLER_SHOOT2, 10, NULL, T_Shoot, &s_hitlershoot3};
static const statetype s_hitlershoot3 = {0, SPR_HITLER_SHOOT3, 10, NULL, T_Shoot, &s_hitlershoot4};
static const statetype s_hitlershoot4 = {0, SPR_HITLER_SHOOT2, 10, NULL, T_Shoot, &s_hitlershoot5};
static const statetype s_hitlershoot5 = {0, SPR_HITLER_SHOOT3, 10, NULL, T_Shoot, &s_hitlershoot6};
static const statetype s_hitlershoot6 = {0, SPR_HITLER_SHOOT2, 10, NULL, T_Shoot, &s_hitlerchase1};

/* Spear of Destiny: the Death Knight's rockets and their smoke */
static const statetype s_hrocket = {1, SPR_HROCKET_1, 3, T_Projectile, A_Smoke, &s_hrocket};
static const statetype s_hsmoke1 = {0, SPR_HSMOKE_1,  3, NULL, NULL, &s_hsmoke2};
static const statetype s_hsmoke2 = {0, SPR_HSMOKE_2,  3, NULL, NULL, &s_hsmoke3};
static const statetype s_hsmoke3 = {0, SPR_HSMOKE_3,  3, NULL, NULL, &s_hsmoke4};
static const statetype s_hsmoke4 = {0, SPR_HSMOKE_4,  3, NULL, NULL, NULL};
static const statetype s_hboom1  = {0, SPR_HBOOM_1,   6, NULL, NULL, &s_hboom2};
static const statetype s_hboom2  = {0, SPR_HBOOM_2,   6, NULL, NULL, &s_hboom3};
static const statetype s_hboom3  = {0, SPR_HBOOM_3,   6, NULL, NULL, NULL};

/* Trans Grosse */
static const statetype s_transstand   = {0, SPR_TRANS_W1, 0,  T_Stand, NULL, &s_transstand};
static const statetype s_transchase1  = {0, SPR_TRANS_W1, 10, T_Chase, NULL, &s_transchase1s};
static const statetype s_transchase1s = {0, SPR_TRANS_W1, 3,  NULL,    NULL, &s_transchase2};
static const statetype s_transchase2  = {0, SPR_TRANS_W2, 8,  T_Chase, NULL, &s_transchase3};
static const statetype s_transchase3  = {0, SPR_TRANS_W3, 10, T_Chase, NULL, &s_transchase3s};
static const statetype s_transchase3s = {0, SPR_TRANS_W3, 3,  NULL,    NULL, &s_transchase4};
static const statetype s_transchase4  = {0, SPR_TRANS_W4, 8,  T_Chase, NULL, &s_transchase1};
static const statetype s_transdie0    = {0, SPR_TRANS_W1,   1,  NULL, A_DeathScream, &s_transdie01};
static statetype       s_transdie01   = {0, SPR_TRANS_W1,   1,  NULL, NULL, &s_transdie1};
static const statetype s_transdie1    = {0, SPR_TRANS_DIE1, 15, NULL, NULL, &s_transdie2};
static const statetype s_transdie2    = {0, SPR_TRANS_DIE2, 15, NULL, NULL, &s_transdie3};
static const statetype s_transdie3    = {0, SPR_TRANS_DIE3, 15, NULL, NULL, &s_transdie4};
static const statetype s_transdie4    = {0, SPR_TRANS_DEAD, 0,  NULL, NULL, &s_transdie4};
static const statetype s_transshoot1  = {0, SPR_TRANS_SHOOT1, 30, NULL, NULL,    &s_transshoot2};
static const statetype s_transshoot2  = {0, SPR_TRANS_SHOOT2, 10, NULL, T_Shoot, &s_transshoot3};
static const statetype s_transshoot3  = {0, SPR_TRANS_SHOOT3, 10, NULL, T_Shoot, &s_transshoot4};
static const statetype s_transshoot4  = {0, SPR_TRANS_SHOOT2, 10, NULL, T_Shoot, &s_transshoot5};
static const statetype s_transshoot5  = {0, SPR_TRANS_SHOOT3, 10, NULL, T_Shoot, &s_transshoot6};
static const statetype s_transshoot6  = {0, SPR_TRANS_SHOOT2, 10, NULL, T_Shoot, &s_transshoot7};
static const statetype s_transshoot7  = {0, SPR_TRANS_SHOOT3, 10, NULL, T_Shoot, &s_transshoot8};
static const statetype s_transshoot8  = {0, SPR_TRANS_SHOOT1, 10, NULL, NULL,    &s_transchase1};

/* the Ubermutant */
static const statetype s_uberstand   = {0, SPR_UBER_W1, 0,  T_Stand, NULL, &s_uberstand};
static const statetype s_uberchase1  = {0, SPR_UBER_W1, 10, T_Chase, NULL, &s_uberchase1s};
static const statetype s_uberchase1s = {0, SPR_UBER_W1, 3,  NULL,    NULL, &s_uberchase2};
static const statetype s_uberchase2  = {0, SPR_UBER_W2, 8,  T_Chase, NULL, &s_uberchase3};
static const statetype s_uberchase3  = {0, SPR_UBER_W3, 10, T_Chase, NULL, &s_uberchase3s};
static const statetype s_uberchase3s = {0, SPR_UBER_W3, 3,  NULL,    NULL, &s_uberchase4};
static const statetype s_uberchase4  = {0, SPR_UBER_W4, 8,  T_Chase, NULL, &s_uberchase1};
static const statetype s_uberdie0    = {0, SPR_UBER_W1,   1,  NULL, A_DeathScream, &s_uberdie01};
static statetype       s_uberdie01   = {0, SPR_UBER_W1,   1,  NULL, NULL, &s_uberdie1};
static const statetype s_uberdie1    = {0, SPR_UBER_DIE1, 15, NULL, NULL, &s_uberdie2};
static const statetype s_uberdie2    = {0, SPR_UBER_DIE2, 15, NULL, NULL, &s_uberdie3};
static const statetype s_uberdie3    = {0, SPR_UBER_DIE3, 15, NULL, NULL, &s_uberdie4};
static const statetype s_uberdie4    = {0, SPR_UBER_DIE4, 15, NULL, NULL, &s_uberdie5};
static const statetype s_uberdie5    = {0, SPR_UBER_DEAD, 0,  NULL, NULL, &s_uberdie5};
static const statetype s_ubershoot1  = {0, SPR_UBER_SHOOT1, 30, NULL, NULL,     &s_ubershoot2};
static const statetype s_ubershoot2  = {0, SPR_UBER_SHOOT2, 12, NULL, T_UShoot, &s_ubershoot3};
static const statetype s_ubershoot3  = {0, SPR_UBER_SHOOT3, 12, NULL, T_UShoot, &s_ubershoot4};
static const statetype s_ubershoot4  = {0, SPR_UBER_SHOOT4, 12, NULL, T_UShoot, &s_ubershoot5};
static const statetype s_ubershoot5  = {0, SPR_UBER_SHOOT3, 12, NULL, T_UShoot, &s_ubershoot6};
static const statetype s_ubershoot6  = {0, SPR_UBER_SHOOT2, 12, NULL, T_UShoot, &s_ubershoot7};
static const statetype s_ubershoot7  = {0, SPR_UBER_SHOOT1, 12, NULL, NULL,     &s_uberchase1};

/* Barnacle Wilhelm */
static const statetype s_willstand   = {0, SPR_WILL_W1, 0,  T_Stand, NULL, &s_willstand};
static const statetype s_willchase1  = {0, SPR_WILL_W1, 10, T_Will,  NULL, &s_willchase1s};
static const statetype s_willchase1s = {0, SPR_WILL_W1, 3,  NULL,    NULL, &s_willchase2};
static const statetype s_willchase2  = {0, SPR_WILL_W2, 8,  T_Will,  NULL, &s_willchase3};
static const statetype s_willchase3  = {0, SPR_WILL_W3, 10, T_Will,  NULL, &s_willchase3s};
static const statetype s_willchase3s = {0, SPR_WILL_W3, 3,  NULL,    NULL, &s_willchase4};
static const statetype s_willchase4  = {0, SPR_WILL_W4, 8,  T_Will,  NULL, &s_willchase1};
static const statetype s_willdeathcam = {0, SPR_WILL_W1, 1, NULL, NULL, &s_willdie1};
static const statetype s_willdie1    = {0, SPR_WILL_W1,   1,  NULL, A_DeathScream, &s_willdie2};
static statetype       s_willdie2    = {0, SPR_WILL_W1,   10, NULL, NULL, &s_willdie3};
static const statetype s_willdie3    = {0, SPR_WILL_DIE1, 10, NULL, NULL, &s_willdie4};
static const statetype s_willdie4    = {0, SPR_WILL_DIE2, 10, NULL, NULL, &s_willdie5};
static const statetype s_willdie5    = {0, SPR_WILL_DIE3, 10, NULL, NULL, &s_willdie6};
static const statetype s_willdie6    = {0, SPR_WILL_DEAD, 20, NULL, NULL, &s_willdie6};
static const statetype s_willshoot1  = {0, SPR_WILL_SHOOT1, 30, NULL, NULL,     &s_willshoot2};
static const statetype s_willshoot2  = {0, SPR_WILL_SHOOT2, 10, NULL, T_Launch, &s_willshoot3};
static const statetype s_willshoot3  = {0, SPR_WILL_SHOOT3, 10, NULL, T_Shoot,  &s_willshoot4};
static const statetype s_willshoot4  = {0, SPR_WILL_SHOOT4, 10, NULL, T_Shoot,  &s_willshoot5};
static const statetype s_willshoot5  = {0, SPR_WILL_SHOOT3, 10, NULL, T_Shoot,  &s_willshoot6};
static const statetype s_willshoot6  = {0, SPR_WILL_SHOOT4, 10, NULL, T_Shoot,  &s_willchase1};

/* the Death Knight */
static const statetype s_deathstand   = {0, SPR_DEATH_W1, 0,  T_Stand, NULL, &s_deathstand};
static const statetype s_deathchase1  = {0, SPR_DEATH_W1, 10, T_Will,  NULL, &s_deathchase1s};
static const statetype s_deathchase1s = {0, SPR_DEATH_W1, 3,  NULL,    NULL, &s_deathchase2};
static const statetype s_deathchase2  = {0, SPR_DEATH_W2, 8,  T_Will,  NULL, &s_deathchase3};
static const statetype s_deathchase3  = {0, SPR_DEATH_W3, 10, T_Will,  NULL, &s_deathchase3s};
static const statetype s_deathchase3s = {0, SPR_DEATH_W3, 3,  NULL,    NULL, &s_deathchase4};
static const statetype s_deathchase4  = {0, SPR_DEATH_W4, 8,  T_Will,  NULL, &s_deathchase1};
static const statetype s_deathdeathcam = {0, SPR_DEATH_W1, 1, NULL, NULL, &s_deathdie1};
static const statetype s_deathdie1    = {0, SPR_DEATH_W1,   1,  NULL, A_DeathScream, &s_deathdie2};
static statetype       s_deathdie2    = {0, SPR_DEATH_W1,   10, NULL, NULL, &s_deathdie3};
static const statetype s_deathdie3    = {0, SPR_DEATH_DIE1, 10, NULL, NULL, &s_deathdie4};
static const statetype s_deathdie4    = {0, SPR_DEATH_DIE2, 10, NULL, NULL, &s_deathdie5};
static const statetype s_deathdie5    = {0, SPR_DEATH_DIE3, 10, NULL, NULL, &s_deathdie6};
static const statetype s_deathdie6    = {0, SPR_DEATH_DIE4, 10, NULL, NULL, &s_deathdie7};
static const statetype s_deathdie7    = {0, SPR_DEATH_DIE5, 10, NULL, NULL, &s_deathdie8};
static const statetype s_deathdie8    = {0, SPR_DEATH_DIE6, 10, NULL, NULL, &s_deathdie9};
static const statetype s_deathdie9    = {0, SPR_DEATH_DEAD, 0,  NULL, NULL, &s_deathdie9};
static const statetype s_deathshoot1  = {0, SPR_DEATH_SHOOT1, 30, NULL, NULL,     &s_deathshoot2};
static const statetype s_deathshoot2  = {0, SPR_DEATH_SHOOT2, 10, NULL, T_Launch, &s_deathshoot3};
static const statetype s_deathshoot3  = {0, SPR_DEATH_SHOOT4, 10, NULL, T_Shoot,  &s_deathshoot4};
static const statetype s_deathshoot4  = {0, SPR_DEATH_SHOOT3, 10, NULL, T_Launch, &s_deathshoot5};
static const statetype s_deathshoot5  = {0, SPR_DEATH_SHOOT4, 10, NULL, T_Shoot,  &s_deathchase1};

/* the Angel of Death, and her sparks */
static const statetype s_angelstand   = {0, SPR_ANGEL_W1, 0,  T_Stand, NULL, &s_angelstand};
static const statetype s_angelchase1  = {0, SPR_ANGEL_W1, 10, T_Will,  NULL, &s_angelchase1s};
static const statetype s_angelchase1s = {0, SPR_ANGEL_W1, 3,  NULL,    NULL, &s_angelchase2};
static const statetype s_angelchase2  = {0, SPR_ANGEL_W2, 8,  T_Will,  NULL, &s_angelchase3};
static const statetype s_angelchase3  = {0, SPR_ANGEL_W3, 10, T_Will,  NULL, &s_angelchase3s};
static const statetype s_angelchase3s = {0, SPR_ANGEL_W3, 3,  NULL,    NULL, &s_angelchase4};
static const statetype s_angelchase4  = {0, SPR_ANGEL_W4, 8,  T_Will,  NULL, &s_angelchase1};
static const statetype s_angeldie1    = {0, SPR_ANGEL_W1,   1,   NULL, A_DeathScream, &s_angeldie11};
static statetype       s_angeldie11   = {0, SPR_ANGEL_W1,   1,   NULL, NULL,      &s_angeldie2};
static const statetype s_angeldie2    = {0, SPR_ANGEL_DIE1, 10,  NULL, A_Slurpie, &s_angeldie3};
static const statetype s_angeldie3    = {0, SPR_ANGEL_DIE2, 10,  NULL, NULL,      &s_angeldie4};
static const statetype s_angeldie4    = {0, SPR_ANGEL_DIE3, 10,  NULL, NULL,      &s_angeldie5};
static const statetype s_angeldie5    = {0, SPR_ANGEL_DIE4, 10,  NULL, NULL,      &s_angeldie6};
static const statetype s_angeldie6    = {0, SPR_ANGEL_DIE5, 10,  NULL, NULL,      &s_angeldie7};
static const statetype s_angeldie7    = {0, SPR_ANGEL_DIE6, 10,  NULL, NULL,      &s_angeldie8};
static const statetype s_angeldie8    = {0, SPR_ANGEL_DIE7, 10,  NULL, NULL,      &s_angeldie9};
static const statetype s_angeldie9    = {0, SPR_ANGEL_DEAD, 130, NULL, A_Victory, &s_angeldie9};
static const statetype s_angelshoot1  = {0, SPR_ANGEL_SHOOT1, 10, NULL, A_StartAttack, &s_angelshoot2};
static const statetype s_angelshoot2  = {0, SPR_ANGEL_SHOOT2, 20, NULL, T_Launch,      &s_angelshoot3};
static const statetype s_angelshoot3  = {0, SPR_ANGEL_SHOOT1, 10, NULL, A_Relaunch,    &s_angelshoot2};
static const statetype s_angeltired   = {0, SPR_ANGEL_TIRED1, 40, NULL, A_Breathing, &s_angeltired2};
static const statetype s_angeltired2  = {0, SPR_ANGEL_TIRED2, 40, NULL, NULL,        &s_angeltired3};
static const statetype s_angeltired3  = {0, SPR_ANGEL_TIRED1, 40, NULL, A_Breathing, &s_angeltired4};
static const statetype s_angeltired4  = {0, SPR_ANGEL_TIRED2, 40, NULL, NULL,        &s_angeltired5};
static const statetype s_angeltired5  = {0, SPR_ANGEL_TIRED1, 40, NULL, A_Breathing, &s_angeltired6};
static const statetype s_angeltired6  = {0, SPR_ANGEL_TIRED2, 40, NULL, NULL,        &s_angeltired7};
static const statetype s_angeltired7  = {0, SPR_ANGEL_TIRED1, 40, NULL, A_Breathing, &s_angelchase1};
static const statetype s_spark1       = {0, SPR_SPARK1, 6, T_Projectile, NULL, &s_spark2};
static const statetype s_spark2       = {0, SPR_SPARK2, 6, T_Projectile, NULL, &s_spark3};
static const statetype s_spark3       = {0, SPR_SPARK3, 6, T_Projectile, NULL, &s_spark4};
static const statetype s_spark4       = {0, SPR_SPARK4, 6, T_Projectile, NULL, &s_spark1};

/* the spectres */
static const statetype s_spectrewait1  = {0, SPR_SPECTRE_W1, 10,  T_Stand,  NULL, &s_spectrewait2};
static const statetype s_spectrewait2  = {0, SPR_SPECTRE_W2, 10,  T_Stand,  NULL, &s_spectrewait3};
static const statetype s_spectrewait3  = {0, SPR_SPECTRE_W3, 10,  T_Stand,  NULL, &s_spectrewait4};
static const statetype s_spectrewait4  = {0, SPR_SPECTRE_W4, 10,  T_Stand,  NULL, &s_spectrewait1};
static const statetype s_spectrechase1 = {0, SPR_SPECTRE_W1, 10,  T_Ghosts, NULL, &s_spectrechase2};
static const statetype s_spectrechase2 = {0, SPR_SPECTRE_W2, 10,  T_Ghosts, NULL, &s_spectrechase3};
static const statetype s_spectrechase3 = {0, SPR_SPECTRE_W3, 10,  T_Ghosts, NULL, &s_spectrechase4};
static const statetype s_spectrechase4 = {0, SPR_SPECTRE_W4, 10,  T_Ghosts, NULL, &s_spectrechase1};
static const statetype s_spectredie1   = {0, SPR_SPECTRE_F1, 10,  NULL, NULL,      &s_spectredie2};
static const statetype s_spectredie2   = {0, SPR_SPECTRE_F2, 10,  NULL, NULL,      &s_spectredie3};
static const statetype s_spectredie3   = {0, SPR_SPECTRE_F3, 10,  NULL, NULL,      &s_spectredie4};
static const statetype s_spectredie4   = {0, SPR_SPECTRE_F4, 300, NULL, NULL,      &s_spectrewake};
static const statetype s_spectrewake   = {0, SPR_SPECTRE_F4, 10,  NULL, A_Dormant, &s_spectrewake};

/* Every state above, so that a saved game can name one by number. The DOS
 * save wrote the far pointers themselves, which only worked because the
 * same executable read them back. Append to this list; never reorder it. */
static const statetype *const all_states[] = {
    &s_grdstand, &s_grdpath1, &s_grdpath1s, &s_grdpath2, &s_grdpath3,
    &s_grdpath3s, &s_grdpath4, &s_grdpain, &s_grdpain1, &s_grdshoot1,
    &s_grdshoot2, &s_grdshoot3, &s_grdchase1, &s_grdchase1s, &s_grdchase2,
    &s_grdchase3, &s_grdchase3s, &s_grdchase4, &s_grddie1, &s_grddie2,
    &s_grddie3, &s_grddie4,
    &s_dogpath1, &s_dogpath1s, &s_dogpath2, &s_dogpath3, &s_dogpath3s,
    &s_dogpath4, &s_dogjump1, &s_dogjump2, &s_dogjump3, &s_dogjump4,
    &s_dogjump5, &s_dogchase1, &s_dogchase1s, &s_dogchase2, &s_dogchase3,
    &s_dogchase3s, &s_dogchase4, &s_dogdie1, &s_dogdie2, &s_dogdie3,
    &s_dogdead,
    &s_ssstand, &s_sspath1, &s_sspath1s, &s_sspath2, &s_sspath3,
    &s_sspath3s, &s_sspath4, &s_sspain, &s_sspain1, &s_ssshoot1,
    &s_ssshoot2, &s_ssshoot3, &s_ssshoot4, &s_ssshoot5, &s_ssshoot6,
    &s_ssshoot7, &s_ssshoot8, &s_ssshoot9, &s_sschase1, &s_sschase1s,
    &s_sschase2, &s_sschase3, &s_sschase3s, &s_sschase4, &s_ssdie1,
    &s_ssdie2, &s_ssdie3, &s_ssdie4,
    &s_bossstand, &s_bosschase1, &s_bosschase1s, &s_bosschase2,
    &s_bosschase3, &s_bosschase3s, &s_bosschase4, &s_bossdie1, &s_bossdie2,
    &s_bossdie3, &s_bossdie4, &s_bossshoot1, &s_bossshoot2, &s_bossshoot3,
    &s_bossshoot4, &s_bossshoot5, &s_bossshoot6, &s_bossshoot7,
    &s_bossshoot8,
    &s_bjrun1, &s_bjrun1s, &s_bjrun2, &s_bjrun3, &s_bjrun3s, &s_bjrun4,
    &s_bjjump1, &s_bjjump2, &s_bjjump3, &s_bjjump4,
    /* the full game's */
    &s_rocket, &s_smoke1, &s_smoke2, &s_smoke3, &s_smoke4,
    &s_boom1, &s_boom2, &s_boom3,
    &s_ofcstand, &s_ofcpath1, &s_ofcpath1s, &s_ofcpath2, &s_ofcpath3,
    &s_ofcpath3s, &s_ofcpath4, &s_ofcpain, &s_ofcpain1, &s_ofcshoot1,
    &s_ofcshoot2, &s_ofcshoot3, &s_ofcchase1, &s_ofcchase1s, &s_ofcchase2,
    &s_ofcchase3, &s_ofcchase3s, &s_ofcchase4, &s_ofcdie1, &s_ofcdie2,
    &s_ofcdie3, &s_ofcdie4, &s_ofcdie5,
    &s_mutstand, &s_mutpath1, &s_mutpath1s, &s_mutpath2, &s_mutpath3,
    &s_mutpath3s, &s_mutpath4, &s_mutpain, &s_mutpain1, &s_mutshoot1,
    &s_mutshoot2, &s_mutshoot3, &s_mutshoot4, &s_mutchase1, &s_mutchase1s,
    &s_mutchase2, &s_mutchase3, &s_mutchase3s, &s_mutchase4, &s_mutdie1,
    &s_mutdie2, &s_mutdie3, &s_mutdie4, &s_mutdie5,
    &s_blinkychase1, &s_blinkychase2, &s_inkychase1, &s_inkychase2,
    &s_pinkychase1, &s_pinkychase2, &s_clydechase1, &s_clydechase2,
    &s_gretelstand, &s_gretelchase1, &s_gretelchase1s, &s_gretelchase2,
    &s_gretelchase3, &s_gretelchase3s, &s_gretelchase4, &s_greteldie1,
    &s_greteldie2, &s_greteldie3, &s_greteldie4, &s_gretelshoot1,
    &s_gretelshoot2, &s_gretelshoot3, &s_gretelshoot4, &s_gretelshoot5,
    &s_gretelshoot6, &s_gretelshoot7, &s_gretelshoot8,
    &s_schabbstand, &s_schabbchase1, &s_schabbchase1s, &s_schabbchase2,
    &s_schabbchase3, &s_schabbchase3s, &s_schabbchase4, &s_schabbdeathcam,
    &s_schabbdie1, &s_schabbdie2, &s_schabbdie3, &s_schabbdie4,
    &s_schabbdie5, &s_schabbdie6, &s_schabbshoot1, &s_schabbshoot2,
    &s_needle1, &s_needle2, &s_needle3, &s_needle4,
    &s_giftstand, &s_giftchase1, &s_giftchase1s, &s_giftchase2,
    &s_giftchase3, &s_giftchase3s, &s_giftchase4, &s_giftdeathcam,
    &s_giftdie1, &s_giftdie2, &s_giftdie3, &s_giftdie4, &s_giftdie5,
    &s_giftdie6, &s_giftshoot1, &s_giftshoot2,
    &s_fatstand, &s_fatchase1, &s_fatchase1s, &s_fatchase2, &s_fatchase3,
    &s_fatchase3s, &s_fatchase4, &s_fatdeathcam, &s_fatdie1, &s_fatdie2,
    &s_fatdie3, &s_fatdie4, &s_fatdie5, &s_fatdie6, &s_fatshoot1,
    &s_fatshoot2, &s_fatshoot3, &s_fatshoot4, &s_fatshoot5, &s_fatshoot6,
    &s_fakestand, &s_fakechase1, &s_fakechase1s, &s_fakechase2,
    &s_fakechase3, &s_fakechase3s, &s_fakechase4, &s_fakedie1, &s_fakedie2,
    &s_fakedie3, &s_fakedie4, &s_fakedie5, &s_fakedie6, &s_fakeshoot1,
    &s_fakeshoot2, &s_fakeshoot3, &s_fakeshoot4, &s_fakeshoot5,
    &s_fakeshoot6, &s_fakeshoot7, &s_fakeshoot8, &s_fakeshoot9, &s_fire1,
    &s_fire2,
    &s_mechastand, &s_mechachase1, &s_mechachase1s, &s_mechachase2,
    &s_mechachase3, &s_mechachase3s, &s_mechachase4, &s_mechadie1,
    &s_mechadie2, &s_mechadie3, &s_mechadie4, &s_mechashoot1,
    &s_mechashoot2, &s_mechashoot3, &s_mechashoot4, &s_mechashoot5,
    &s_mechashoot6,
    &s_hitlerchase1, &s_hitlerchase1s, &s_hitlerchase2, &s_hitlerchase3,
    &s_hitlerchase3s, &s_hitlerchase4, &s_hitlerdeathcam, &s_hitlerdie1,
    &s_hitlerdie2, &s_hitlerdie3, &s_hitlerdie4, &s_hitlerdie5,
    &s_hitlerdie6, &s_hitlerdie7, &s_hitlerdie8, &s_hitlerdie9,
    &s_hitlerdie10, &s_hitlershoot1, &s_hitlershoot2, &s_hitlershoot3,
    &s_hitlershoot4, &s_hitlershoot5, &s_hitlershoot6,
    /* Spear of Destiny's */
    &s_hrocket, &s_hsmoke1, &s_hsmoke2, &s_hsmoke3, &s_hsmoke4,
    &s_hboom1, &s_hboom2, &s_hboom3,
    &s_transstand, &s_transchase1, &s_transchase1s, &s_transchase2,
    &s_transchase3, &s_transchase3s, &s_transchase4, &s_transdie0,
    &s_transdie01, &s_transdie1, &s_transdie2, &s_transdie3, &s_transdie4,
    &s_transshoot1, &s_transshoot2, &s_transshoot3, &s_transshoot4,
    &s_transshoot5, &s_transshoot6, &s_transshoot7, &s_transshoot8,
    &s_uberstand, &s_uberchase1, &s_uberchase1s, &s_uberchase2,
    &s_uberchase3, &s_uberchase3s, &s_uberchase4, &s_uberdie0, &s_uberdie01,
    &s_uberdie1, &s_uberdie2, &s_uberdie3, &s_uberdie4, &s_uberdie5,
    &s_ubershoot1, &s_ubershoot2, &s_ubershoot3, &s_ubershoot4,
    &s_ubershoot5, &s_ubershoot6, &s_ubershoot7,
    &s_willstand, &s_willchase1, &s_willchase1s, &s_willchase2,
    &s_willchase3, &s_willchase3s, &s_willchase4, &s_willdeathcam,
    &s_willdie1, &s_willdie2, &s_willdie3, &s_willdie4, &s_willdie5,
    &s_willdie6, &s_willshoot1, &s_willshoot2, &s_willshoot3, &s_willshoot4,
    &s_willshoot5, &s_willshoot6,
    &s_deathstand, &s_deathchase1, &s_deathchase1s, &s_deathchase2,
    &s_deathchase3, &s_deathchase3s, &s_deathchase4, &s_deathdeathcam,
    &s_deathdie1, &s_deathdie2, &s_deathdie3, &s_deathdie4, &s_deathdie5,
    &s_deathdie6, &s_deathdie7, &s_deathdie8, &s_deathdie9, &s_deathshoot1,
    &s_deathshoot2, &s_deathshoot3, &s_deathshoot4, &s_deathshoot5,
    &s_angelstand, &s_angelchase1, &s_angelchase1s, &s_angelchase2,
    &s_angelchase3, &s_angelchase3s, &s_angelchase4, &s_angeldie1,
    &s_angeldie11, &s_angeldie2, &s_angeldie3, &s_angeldie4, &s_angeldie5,
    &s_angeldie6, &s_angeldie7, &s_angeldie8, &s_angeldie9, &s_angelshoot1,
    &s_angelshoot2, &s_angelshoot3, &s_angeltired, &s_angeltired2,
    &s_angeltired3, &s_angeltired4, &s_angeltired5, &s_angeltired6,
    &s_angeltired7, &s_spark1, &s_spark2, &s_spark3, &s_spark4,
    &s_spectrewait1, &s_spectrewait2, &s_spectrewait3, &s_spectrewait4,
    &s_spectrechase1, &s_spectrechase2, &s_spectrechase3, &s_spectrechase4,
    &s_spectredie1, &s_spectredie2, &s_spectredie3, &s_spectredie4,
    &s_spectrewake,
};

#define NUM_STATES  ((int)(sizeof all_states / sizeof all_states[0]))

int actor_state_number(const statetype *state)
{
    for (int i = 0; i < NUM_STATES; i++)
        if (all_states[i] == state)
            return i;
    return -1;
}

const statetype *actor_state(int number)
{
    return number >= 0 && number < NUM_STATES ? all_states[number] : NULL;
}

/* ------------------------------------------------------------------ */
/* Tables and small helpers                                            */
/* ------------------------------------------------------------------ */

static const uint8_t opposite[9] =
    {west, southwest, south, southeast, east, northeast, north, northwest, nodir};

static const uint8_t diagonal[9][9] = {
    /* east  */ {nodir, nodir, northeast, nodir, nodir, nodir, southeast, nodir, nodir},
                {nodir, nodir, nodir, nodir, nodir, nodir, nodir, nodir, nodir},
    /* north */ {northeast, nodir, nodir, nodir, northwest, nodir, nodir, nodir, nodir},
                {nodir, nodir, nodir, nodir, nodir, nodir, nodir, nodir, nodir},
    /* west  */ {nodir, nodir, northwest, nodir, nodir, nodir, southwest, nodir, nodir},
                {nodir, nodir, nodir, nodir, nodir, nodir, nodir, nodir, nodir},
    /* south */ {southeast, nodir, nodir, nodir, southwest, nodir, nodir, nodir, nodir},
                {nodir, nodir, nodir, nodir, nodir, nodir, nodir, nodir, nodir},
                {nodir, nodir, nodir, nodir, nodir, nodir, nodir, nodir, nodir}
};

/* enemy_t, for starthitpoints[] */
enum {
    en_guard, en_officer, en_ss, en_dog, en_boss, en_schabbs, en_fake,
    en_hitler, en_mutant, en_blinky, en_clyde, en_pinky, en_inky, en_gretel,
    en_gift, en_fat, en_spectre, en_angel, en_trans, en_uber, en_will,
    en_death, NUMENEMIES
};

static const int16_t starthitpoints[4][NUMENEMIES] = {
    /* baby mode */
    { 25, 50, 100, 1, 850,  850,  200, 800,  45, 25, 25, 25, 25,
      850,  850,  850,  5,  1450, 850,  1050, 950,  1250 },
    /* don't hurt me */
    { 25, 50, 100, 1, 950,  950,  300, 950,  55, 25, 25, 25, 25,
      950,  950,  950,  10, 1550, 950,  1150, 1050, 1350 },
    /* bring 'em on */
    { 25, 50, 100, 1, 1050, 1550, 400, 1050, 55, 25, 25, 25, 25,
      1050, 1050, 1050, 15, 1650, 1050, 1250, 1150, 1450 },
    /* death incarnate */
    { 25, 50, 100, 1, 1200, 2400, 500, 1200, 65, 25, 25, 25, 25,
      1200, 1200, 1200, 25, 2000, 1200, 1400, 1300, 1600 },
};

static int start_hitpoints(int which)
{
    return starthitpoints[gamestate.difficulty & 3][which];
}

static void new_state(objtype *ob, const statetype *state)
{
    ob->state = state;
    ob->ticcount = state->tictime;
}

static void play_sound_loc_actor(int sound, objtype *ob)
{
    sd_play_sound_loc_global(sound, ob->x, ob->y);
}

static bool in_map(int x, int y)
{
    return x >= 0 && y >= 0 && x < MAPSIZE && y < MAPSIZE;
}

/* The player's tile distance, as the chase routines measure it */
static int tile_dist(const objtype *ob)
{
    const int dx = abs(ob->tilex - player.tilex);
    const int dy = abs(ob->tiley - player.tiley);
    return dx > dy ? dx : dy;
}

/* The angle from ob to the player, in the DOS game's degrees, worked out as
 * T_SchabbThrow() and its fellows did it. */
static int angle_to_player(const objtype *ob)
{
    const int32_t deltax = player.x - ob->x;
    const int32_t deltay = ob->y - player.y;
    float angle = (float)atan2((float)deltay, (float)deltax);
    if (angle < 0)
        angle = (float)(M_PI * 2 + angle);
    return (int)(angle / (M_PI * 2) * ANGLES);
}

/* ------------------------------------------------------------------ */
/* The actor list - InitActorList, GetNewActor, RemoveObj              */
/* ------------------------------------------------------------------ */

void actors_init(void)
{
    memset(objlist, 0, sizeof(objlist));
    memset(actorat, 0, sizeof(actorat));
    numobjs = 0;
    num_free = MAXACTORS;
    for (int i = 0; i < MAXACTORS; i++)
        free_slots[i] = (uint8_t)(MAXACTORS - 1 - i);   /* slot 0 first */
    victory_done = false;
    deathcam_started = deathcam = false;
}

static objtype *get_new_actor(void)
{
    objtype *ob;

    if (!num_free)
        return NULL;                        /* DOS quits; drop it instead */
    ob = &objlist[free_slots[--num_free]];
    memset(ob, 0, sizeof *ob);
    obj_order[numobjs++] = (uint8_t)(ob - objlist);
    return ob;
}

static void remove_obj(objtype *ob)
{
    const int slot = (int)(ob - objlist);

    ob->state = NULL;
    for (int i = 0; i < numobjs; i++)
        if (obj_order[i] == slot) {
            memmove(&obj_order[i], &obj_order[i + 1], numobjs - 1 - i);
            numobjs--;
            free_slots[num_free++] = (uint8_t)slot;
            return;
        }
}

int actor_free_slots(uint8_t *slots)
{
    for (int i = 0; i < num_free; i++)
        slots[i] = free_slots[num_free - 1 - i];
    return num_free;
}

/* A loaded game's list: the slots in order, then the free ones, next first.
 * Every slot has to be one or the other. */
bool actor_set_list(const uint8_t *order, int count, const uint8_t *free_list,
                    int nfree)
{
    uint8_t seen[MAXACTORS] = { 0 };

    if (count + nfree != MAXACTORS)
        return false;
    for (int i = 0; i < count; i++)
        if (order[i] >= MAXACTORS || seen[order[i]]++)
            return false;
    for (int i = 0; i < nfree; i++)
        if (free_list[i] >= MAXACTORS || seen[free_list[i]]++)
            return false;
    memcpy(obj_order, order, count);
    numobjs = count;
    num_free = nfree;
    for (int i = 0; i < nfree; i++)
        free_slots[i] = free_list[nfree - 1 - i];
    return true;
}

/* ------------------------------------------------------------------ */
/* Spawning - SpawnNewObj, SpawnStand, SpawnPatrol, SpawnDeadGuard,    */
/* the bosses, and the actor half of ScanInfoPlane                     */
/* ------------------------------------------------------------------ */

static objtype *spawn_new_obj(int tilex, int tiley, const statetype *state)
{
    objtype *ob = get_new_actor();

    if (!ob)
        return NULL;

    ob->state    = state;
    ob->ticcount = state->tictime ? us_rndt() % state->tictime : 0;

    /* SpawnNewObj() starts a timed state at US_RndT() % tictime tics, and
     * DoActor() treats a count of 0 as a state that never ends. About one
     * spawn in twelve or twenty, the actor would walk its whole route on
     * its first frame - the DOS game's "moonwalking" guards - until a new
     * state (seeing the player, taking a hit) reset the count. Start those
     * with a whole state instead. The random number is still drawn, so the
     * rest of the game's random sequence is unchanged. A demo keeps the
     * moonwalkers: it was recorded with them. */
    if (state->tictime && !ob->ticcount && !dos_exact)
        ob->ticcount = state->tictime;
    ob->tilex    = tilex;
    ob->tiley    = tiley;
    ob->x        = ((fixed)tilex << TILESHIFT) + TILEGLOBAL / 2;
    ob->y        = ((fixed)tiley << TILESHIFT) + TILEGLOBAL / 2;
    ob->dir      = nodir;
    actorat[tiley][tilex] = ob;
    ob->areanumber = area_at(tilex, tiley);
    return ob;
}

static void spawn_stand(int obclass, int which, const statetype *state,
                        int tilex, int tiley, int dir, bool ambush)
{
    objtype *ob = spawn_new_obj(tilex, tiley, state);

    if (!ob)
        return;
    ob->speed = SPDPATROL;
    gamestate.killtotal++;
    if (ambush)
        ob->flags |= FL_AMBUSH;             /* level.c already fixed the area */
    ob->obclass   = obclass;
    ob->hitpoints = start_hitpoints(which);
    ob->dir       = dir * 2;
    ob->flags    |= FL_SHOOTABLE;
}

static void spawn_patrol(int obclass, int which, const statetype *state,
                         int tilex, int tiley, int dir)
{
    objtype *ob = spawn_new_obj(tilex, tiley, state);

    if (!ob)
        return;
    ob->speed     = obclass == dogobj ? SPDDOG : SPDPATROL;
    gamestate.killtotal++;
    ob->obclass   = obclass;
    ob->dir       = dir * 2;
    ob->hitpoints = start_hitpoints(which);
    ob->distance  = TILEGLOBAL;
    ob->flags    |= FL_SHOOTABLE;
    ob->active    = true;

    /* The patrol is already on its way to the next tile. */
    actorat[ob->tiley][ob->tilex] = NULL;
    switch (dir) {
    case 0: ob->tilex++; break;
    case 1: ob->tiley--; break;
    case 2: ob->tilex--; break;
    case 3: ob->tiley++; break;
    }
    if (in_map(ob->tilex, ob->tiley))
        actorat[ob->tiley][ob->tilex] = ob;
}

/* SpawnBoss() and the rest: shootable, waiting in ambush. The Wolfenstein
 * bosses walk at patrol speed and face a way of their own; Spear of
 * Destiny's leave both to FirstSighting(). */
static objtype *spawn_boss(int obclass, int which, const statetype *state,
                           int tilex, int tiley, int dir, bool walks)
{
    objtype *ob = spawn_new_obj(tilex, tiley, state);

    if (!ob)
        return NULL;
    if (walks)
        ob->speed = SPDPATROL;
    ob->obclass   = obclass;
    ob->hitpoints = start_hitpoints(which);
    if (walks)
        ob->dir = dir;
    ob->flags    |= FL_SHOOTABLE | FL_AMBUSH;
    gamestate.killtotal++;
    return ob;
}

/* The pause for a boss's last words: as long as the recording, where one
 * will play - SpawnSchabbs() and its fellows */
static void last_words(statetype *state, int digitized, int adlib)
{
    state->tictime = sd_digi_on ? digitized : adlib;
}

static void spawn_ghost(const statetype *state, int tilex, int tiley)
{
    objtype *ob = spawn_new_obj(tilex, tiley, state);

    if (!ob)
        return;
    ob->obclass = ghostobj;
    ob->speed   = SPDDOG;
    ob->dir     = east;
    ob->flags  |= FL_AMBUSH;
    gamestate.killtotal++;
}

void spawn_map_actor(int tile, int tilex, int tiley, bool ambush)
{
    /* The bosses have tiles of their own, which differ between the games;
     * a tile the game does not use spawns nothing. */
    if (wolf_version != WV_SPEAR) {
        switch (tile) {
        case 214:
            spawn_boss(bossobj, en_boss, &s_bossstand, tilex, tiley, south, true);
            return;
        case 197:
            spawn_boss(gretelobj, en_gretel, &s_gretelstand, tilex, tiley, north, true);
            return;
        case 215:
            last_words(&s_giftdie2, 140, 5);
            spawn_boss(giftobj, en_gift, &s_giftstand, tilex, tiley, north, true);
            return;
        case 179:
            last_words(&s_fatdie2, 140, 5);
            spawn_boss(fatobj, en_fat, &s_fatstand, tilex, tiley, south, true);
            return;
        case 196:
            last_words(&s_schabbdie2, 140, 5);
            spawn_boss(schabbobj, en_schabbs, &s_schabbstand, tilex, tiley, south, true);
            return;
        case 160:
            last_words(&s_hitlerdie2, 140, 5);
            spawn_boss(fakeobj, en_fake, &s_fakestand, tilex, tiley, north, true);
            return;
        case 178:
            last_words(&s_hitlerdie2, 140, 5);
            spawn_boss(mechahitlerobj, en_hitler, &s_mechastand, tilex, tiley, south, true);
            return;
        case 224: spawn_ghost(&s_blinkychase1, tilex, tiley); return;
        case 225: spawn_ghost(&s_clydechase1, tilex, tiley);  return;
        case 226: spawn_ghost(&s_pinkychase1, tilex, tiley);  return;
        case 227: spawn_ghost(&s_inkychase1, tilex, tiley);   return;
        }
    } else {
        switch (tile) {
        case 106:
            spawn_boss(spectreobj, en_spectre, &s_spectrewait1, tilex, tiley, 0, false);
            return;
        case 107:
            last_words(&s_angeldie11, 105, 1);
            spawn_boss(angelobj, en_angel, &s_angelstand, tilex, tiley, 0, false);
            return;
        case 125:
            last_words(&s_transdie01, 105, 1);
            spawn_boss(transobj, en_trans, &s_transstand, tilex, tiley, 0, false);
            return;
        case 142:
            last_words(&s_uberdie01, 70, 1);
            spawn_boss(uberobj, en_uber, &s_uberstand, tilex, tiley, 0, false);
            return;
        case 143:
            last_words(&s_willdie2, 70, 10);
            spawn_boss(willobj, en_will, &s_willstand, tilex, tiley, 0, false);
            return;
        case 161:
            last_words(&s_deathdie2, 105, 10);
            spawn_boss(deathobj, en_death, &s_deathstand, tilex, tiley, 0, false);
            return;
        }
    }

    if (tile == 124) {                      /* SpawnDeadGuard() */
        objtype *ob = spawn_new_obj(tilex, tiley, &s_grddie4);
        if (ob)
            ob->obclass = inertobj;
        return;
    }

    /* Mutants come in three copies, 18 tiles apart, for the difficulty that
     * adds them: base (all), +18 (Bring 'em on! and up), +36 (Death
     * incarnate). The switch in ScanInfoPlane() falls through to the base
     * case after checking; this does the same with arithmetic. */
    if (tile >= 216 && tile <= 259) {
        if (tile >= 252) {
            if (gamestate.difficulty < gd_hard)
                return;
            tile -= 18;
        }
        if (tile >= 234) {
            if (gamestate.difficulty < gd_medium)
                return;
            tile -= 18;
        }
        if (tile <= 219)
            spawn_stand(mutantobj, en_mutant, &s_mutstand, tilex, tiley, tile - 216, ambush);
        else if (tile <= 223)
            spawn_patrol(mutantobj, en_mutant, &s_mutpath1, tilex, tiley, tile - 220);
        return;
    }

    /* Everyone else comes in three copies 36 tiles apart - less the tiles
     * in those runs that are bosses' of their own. */
    if (tile >= 180 && tile <= 213) {
        if (tile == 196 || tile == 197 || gamestate.difficulty < gd_hard)
            return;
        tile -= 36;
    }
    if (tile >= 144 && tile <= 177) {
        if (tile == 160 || tile == 161 || gamestate.difficulty < gd_medium)
            return;
        tile -= 36;
    }

    if (tile >= 108 && tile <= 111)
        spawn_stand(guardobj, en_guard, &s_grdstand, tilex, tiley, tile - 108, ambush);
    else if (tile >= 112 && tile <= 115)
        spawn_patrol(guardobj, en_guard, &s_grdpath1, tilex, tiley, tile - 112);
    else if (tile >= 116 && tile <= 119)
        spawn_stand(officerobj, en_officer, &s_ofcstand, tilex, tiley, tile - 116, ambush);
    else if (tile >= 120 && tile <= 123)
        spawn_patrol(officerobj, en_officer, &s_ofcpath1, tilex, tiley, tile - 120);
    else if (tile >= 126 && tile <= 129)
        spawn_stand(ssobj, en_ss, &s_ssstand, tilex, tiley, tile - 126, ambush);
    else if (tile >= 130 && tile <= 133)
        spawn_patrol(ssobj, en_ss, &s_sspath1, tilex, tiley, tile - 130);
    else if (tile >= 138 && tile <= 141)
        spawn_patrol(dogobj, en_dog, &s_dogpath1, tilex, tiley, tile - 138);
    /* Dog stands (134..137) are not handled by SpawnStand() at all in the
     * DOS code, and no map has one. */
}

/* ------------------------------------------------------------------ */
/* Movement - TryWalk, SelectDodgeDir, SelectChaseDir, SelectRunDir,   */
/* MoveObj                                                             */
/* ------------------------------------------------------------------ */

/* CHECKDIAG: anything solid, or a live actor, blocks. */
static bool check_diag(int x, int y)
{
    if (!in_map(x, y) || blockmap[y][x])
        return false;
    if (actorat[y][x] && (actorat[y][x]->flags & FL_SHOOTABLE))
        return false;
    return true;
}

/* CHECKSIDE: as CHECKDIAG, but a closed door is a door to open, not a
 * wall. The door's number comes back through *doornum. */
static bool check_side(int x, int y, int *doornum)
{
    if (!in_map(x, y))
        return false;
    if (actorat[y][x] && (actorat[y][x]->flags & FL_SHOOTABLE))
        return false;
    if (blockmap[y][x]) {
        int t = tilemap[y][x];
        if ((t & 0xc0) != 0x80)
            return false;                   /* wall, scenery, push wall */
        *doornum = t & 0x3f;
    }
    return true;
}

static bool try_walk(objtype *ob)
{
    static const int8_t mdx[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };
    static const int8_t mdy[8] = { 0, -1, -1, -1, 0, 1, 1, 1 };
    int doornum = -1;
    int x = ob->tilex, y = ob->tiley;
    /* the dog and the fake Hitler never open doors */
    bool diag = ob->obclass == dogobj || ob->obclass == fakeobj;

    if (ob->dir >= nodir)
        return false;

    if (ob->obclass == inertobj) {
        /* TryWalk() moves an inert object without looking */
        ob->tilex += mdx[ob->dir];
        ob->tiley += mdy[ob->dir];
    } else {
        switch (ob->dir) {
        case north:
            if (diag ? !check_diag(x, y - 1) : !check_side(x, y - 1, &doornum))
                return false;
            ob->tiley--;
            break;
        case northeast:
            if (!check_diag(x + 1, y - 1) || !check_diag(x + 1, y) || !check_diag(x, y - 1))
                return false;
            ob->tilex++; ob->tiley--;
            break;
        case east:
            if (diag ? !check_diag(x + 1, y) : !check_side(x + 1, y, &doornum))
                return false;
            ob->tilex++;
            break;
        case southeast:
            if (!check_diag(x + 1, y + 1) || !check_diag(x + 1, y) || !check_diag(x, y + 1))
                return false;
            ob->tilex++; ob->tiley++;
            break;
        case south:
            if (diag ? !check_diag(x, y + 1) : !check_side(x, y + 1, &doornum))
                return false;
            ob->tiley++;
            break;
        case southwest:
            if (!check_diag(x - 1, y + 1) || !check_diag(x - 1, y) || !check_diag(x, y + 1))
                return false;
            ob->tilex--; ob->tiley++;
            break;
        case west:
            if (diag ? !check_diag(x - 1, y) : !check_side(x - 1, y, &doornum))
                return false;
            ob->tilex--;
            break;
        case northwest:
            if (!check_diag(x - 1, y - 1) || !check_diag(x - 1, y) || !check_diag(x, y - 1))
                return false;
            ob->tilex--; ob->tiley--;
            break;
        }
    }

    if (doornum != -1) {
        open_door(doornum);
        ob->distance = -doornum - 1;
        return true;
    }

    if (in_map(ob->tilex, ob->tiley))
        ob->areanumber = area_at(ob->tilex, ob->tiley);
    ob->distance = TILEGLOBAL;
    return true;
}

static void select_dodge_dir(objtype *ob)
{
    int deltax, deltay, i;
    uint8_t dirtry[5], turnaround, tdir;

    if (ob->flags & FL_FIRSTATTACK) {
        /* turning around is only ok the very first time after noticing */
        turnaround = nodir;
        ob->flags &= ~FL_FIRSTATTACK;
    } else
        turnaround = opposite[ob->dir];

    deltax = player.tilex - ob->tilex;
    deltay = player.tiley - ob->tiley;

    if (deltax > 0) { dirtry[1] = east;  dirtry[3] = west; }
    else            { dirtry[1] = west;  dirtry[3] = east; }
    if (deltay > 0) { dirtry[2] = south; dirtry[4] = north; }
    else            { dirtry[2] = north; dirtry[4] = south; }

    /* randomize a bit for dodging */
    if (abs(deltax) > abs(deltay)) {
        tdir = dirtry[1]; dirtry[1] = dirtry[2]; dirtry[2] = tdir;
        tdir = dirtry[3]; dirtry[3] = dirtry[4]; dirtry[4] = tdir;
    }
    if (us_rndt() < 128) {
        tdir = dirtry[1]; dirtry[1] = dirtry[2]; dirtry[2] = tdir;
        tdir = dirtry[3]; dirtry[3] = dirtry[4]; dirtry[4] = tdir;
    }

    dirtry[0] = diagonal[dirtry[1]][dirtry[2]];

    for (i = 0; i < 5; i++) {
        if (dirtry[i] == nodir || dirtry[i] == turnaround)
            continue;
        ob->dir = dirtry[i];
        if (try_walk(ob))
            return;
    }

    if (turnaround != nodir) {
        ob->dir = turnaround;
        if (try_walk(ob))
            return;
    }

    ob->dir = nodir;
}

static void select_chase_dir(objtype *ob)
{
    int deltax, deltay;
    uint8_t d[3], tdir, olddir, turnaround;

    olddir = ob->dir;
    turnaround = opposite[olddir];

    deltax = player.tilex - ob->tilex;
    deltay = player.tiley - ob->tiley;

    d[1] = nodir;
    d[2] = nodir;

    if (deltax > 0)      d[1] = east;
    else if (deltax < 0) d[1] = west;
    if (deltay > 0)      d[2] = south;
    else if (deltay < 0) d[2] = north;

    if (abs(deltay) > abs(deltax)) {
        tdir = d[1]; d[1] = d[2]; d[2] = tdir;
    }

    if (d[1] == turnaround) d[1] = nodir;
    if (d[2] == turnaround) d[2] = nodir;

    if (d[1] != nodir) {
        ob->dir = d[1];
        if (try_walk(ob))
            return;
    }
    if (d[2] != nodir) {
        ob->dir = d[2];
        if (try_walk(ob))
            return;
    }

    /* no direct path to the player, so pick another direction */
    if (olddir != nodir) {
        ob->dir = olddir;
        if (try_walk(ob))
            return;
    }

    /* randomly determine direction of search - note that north..west in
     * the dirtype enum takes in northwest too, as in the original */
    if (us_rndt() > 128) {
        for (tdir = north; tdir <= west; tdir++) {
            if (tdir != turnaround) {
                ob->dir = tdir;
                if (try_walk(ob))
                    return;
            }
        }
    } else {
        for (tdir = west; tdir >= north; tdir--) {
            if (tdir != turnaround) {
                ob->dir = tdir;
                if (try_walk(ob))
                    return;
            }
        }
    }

    if (turnaround != nodir) {
        ob->dir = turnaround;
        if (try_walk(ob))
            return;
    }

    ob->dir = nodir;                        /* can't move */
}

/* SelectRunDir(): away from the player, for the bosses who throw things
 * and keep their distance. */
static void select_run_dir(objtype *ob)
{
    const int deltax = player.tilex - ob->tilex;
    const int deltay = player.tiley - ob->tiley;
    uint8_t d[3], tdir;

    d[1] = deltax < 0 ? east : west;
    d[2] = deltay < 0 ? south : north;

    if (abs(deltay) > abs(deltax)) {
        tdir = d[1]; d[1] = d[2]; d[2] = tdir;
    }

    ob->dir = d[1];
    if (try_walk(ob))
        return;                             /* either moved forward or attacked */
    ob->dir = d[2];
    if (try_walk(ob))
        return;

    /* there is no direct path to the player, so pick another direction */
    if (us_rndt() > 128) {
        for (tdir = north; tdir <= west; tdir++) {
            ob->dir = tdir;
            if (try_walk(ob))
                return;
        }
    } else {
        for (tdir = west; tdir >= north; tdir--) {
            ob->dir = tdir;
            if (try_walk(ob))
                return;
        }
    }

    ob->dir = nodir;                        /* can't move */
}

/* Moves ob along its direction; actors are not allowed inside the player,
 * and a ghost or spectre that runs into them hurts. */
static void move_obj(objtype *ob, int32_t move)
{
    static const int8_t mdx[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };
    static const int8_t mdy[8] = { 0, -1, -1, -1, 0, 1, 1, 1 };

    if (ob->dir >= nodir)
        return;

    ob->x += mdx[ob->dir] * move;
    ob->y += mdy[ob->dir] * move;

    if (areabyplayer[ob->areanumber]) {
        int32_t deltax = ob->x - player.x;
        int32_t deltay = ob->y - player.y;
        if (deltax >= -MINACTORDIST && deltax <= MINACTORDIST
            && deltay >= -MINACTORDIST && deltay <= MINACTORDIST) {
            if (ob->obclass == ghostobj || ob->obclass == spectreobj)
                take_damage(think_tics * 2, ob);
            /* back up */
            ob->x -= mdx[ob->dir] * move;
            ob->y -= mdy[ob->dir] * move;
            return;
        }
    }

    ob->distance -= move;
}

/* ------------------------------------------------------------------ */
/* Sight - CheckLine, CheckSight, FirstSighting, SightPlayer           */
/* ------------------------------------------------------------------ */

/* A tile the line of sight can pass: floor, or a door open far enough. The
 * intercept is in 1/256 tile but still carries the tile number, so it is
 * compared against doorposition as the DOS code does - which means any
 * door opened more than a quarter of the way is see-through. */
static bool sight_passes(int x, int y, int32_t intercept)
{
    int value;

    if (!in_map(x, y))
        return false;
    value = tilemap[y][x];
    if (!value)
        return true;
    if ((value & 0xc0) != 0x80)             /* walls, jambs and push walls */
        return false;
    return (uint16_t)intercept <= doorposition[value & 0x3f];
}

static bool check_line(objtype *ob)
{
    int x1 = ob->x >> 8, y1 = ob->y >> 8;
    int xt1 = x1 >> 8, yt1 = y1 >> 8;
    int x2 = player.x >> 8, y2 = player.y >> 8;
    int xt2 = player.tilex, yt2 = player.tiley;
    int xdist, ydist, partial, delta, deltafrac, x, y;
    int32_t ltemp, xstep, ystep, xfrac, yfrac;

    xdist = abs(xt2 - xt1);
    if (xdist > 0) {
        if (xt2 > xt1) { partial = 256 - (x1 & 0xff); xstep = 1; }
        else           { partial = x1 & 0xff;         xstep = -1; }

        deltafrac = abs(x2 - x1);
        delta = y2 - y1;
        ltemp = (int32_t)delta * 256 / (deltafrac ? deltafrac : 1);
        ystep = ltemp > 0x7fff ? 0x7fff : ltemp < -0x7fff ? -0x7fff : ltemp;
        yfrac = y1 + ((ystep * partial) >> 8);

        x = xt1 + xstep;
        xt2 += xstep;
        do {
            y = yfrac >> 8;
            yfrac += ystep;
            if (!sight_passes(x, y, yfrac - ystep / 2))
                return false;
            x += xstep;
        } while (x != xt2);
    }

    ydist = abs(yt2 - yt1);
    if (ydist > 0) {
        if (yt2 > yt1) { partial = 256 - (y1 & 0xff); ystep = 1; }
        else           { partial = y1 & 0xff;         ystep = -1; }

        deltafrac = abs(y2 - y1);
        delta = x2 - x1;
        ltemp = (int32_t)delta * 256 / (deltafrac ? deltafrac : 1);
        xstep = ltemp > 0x7fff ? 0x7fff : ltemp < -0x7fff ? -0x7fff : ltemp;
        xfrac = x1 + ((xstep * partial) >> 8);

        y = yt1 + ystep;
        yt2 += ystep;
        do {
            x = xfrac >> 8;
            xfrac += xstep;
            if (!sight_passes(x, y, xfrac - xstep / 2))
                return false;
            y += ystep;
        } while (y != yt2);
    }

    return true;
}

static bool check_sight(objtype *ob)
{
    int32_t deltax, deltay;

    if (!areabyplayer[ob->areanumber])
        return false;

    /* if the player is real close, sight is automatic */
    deltax = player.x - ob->x;
    deltay = player.y - ob->y;
    if (deltax > -MINSIGHT && deltax < MINSIGHT
        && deltay > -MINSIGHT && deltay < MINSIGHT)
        return true;

    /* see if they are looking in the right direction */
    switch (ob->dir) {
    case north: if (deltay > 0) return false; break;
    case east:  if (deltax < 0) return false; break;
    case south: if (deltay < 0) return false; break;
    case west:  if (deltax > 0) return false; break;
    }

    return check_line(ob);
}

static void first_sighting(objtype *ob)
{
    switch (ob->obclass) {
    case guardobj:
        play_sound_loc_actor(HALTSND, ob);
        new_state(ob, &s_grdchase1);
        ob->speed *= 3;
        break;
    case officerobj:
        play_sound_loc_actor(SPIONSND, ob);
        new_state(ob, &s_ofcchase1);
        ob->speed *= 5;
        break;
    case mutantobj:
        new_state(ob, &s_mutchase1);
        ob->speed *= 3;
        break;
    case ssobj:
        play_sound_loc_actor(SCHUTZADSND, ob);
        new_state(ob, &s_sschase1);
        ob->speed *= 4;
        break;
    case dogobj:
        play_sound_loc_actor(DOGBARKSND, ob);
        new_state(ob, &s_dogchase1);
        ob->speed *= 2;
        break;
    case bossobj:
        sd_play_sound(GUTENTAGSND);
        new_state(ob, &s_bosschase1);
        ob->speed = SPDPATROL * 3;
        break;
    case gretelobj:
        sd_play_sound(KEINSND);
        new_state(ob, &s_gretelchase1);
        ob->speed *= 3;
        break;
    case giftobj:
        sd_play_sound(EINESND);
        new_state(ob, &s_giftchase1);
        ob->speed *= 3;
        break;
    case fatobj:
        sd_play_sound(ERLAUBENSND);
        new_state(ob, &s_fatchase1);
        ob->speed *= 3;
        break;
    case schabbobj:
        sd_play_sound(SCHABBSHASND);
        new_state(ob, &s_schabbchase1);
        ob->speed *= 3;
        break;
    case fakeobj:
        sd_play_sound(TOT_HUNDSND);
        new_state(ob, &s_fakechase1);
        ob->speed *= 3;
        break;
    case mechahitlerobj:
        sd_play_sound(DIESND);
        new_state(ob, &s_mechachase1);
        ob->speed *= 3;
        break;
    case realhitlerobj:
        sd_play_sound(DIESND);
        new_state(ob, &s_hitlerchase1);
        ob->speed *= 5;
        break;
    case ghostobj:
        new_state(ob, &s_blinkychase1);
        ob->speed *= 2;
        break;
    case spectreobj:
        sd_play_sound(GHOSTSIGHTSND);
        new_state(ob, &s_spectrechase1);
        ob->speed = 800;
        break;
    case angelobj:
        sd_play_sound(ANGELSIGHTSND);
        new_state(ob, &s_angelchase1);
        ob->speed = 1536;
        break;
    case transobj:
        sd_play_sound(TRANSSIGHTSND);
        new_state(ob, &s_transchase1);
        ob->speed = 1536;
        break;
    case uberobj:
        new_state(ob, &s_uberchase1);
        ob->speed = 3000;
        break;
    case willobj:
        sd_play_sound(WILHELMSIGHTSND);
        new_state(ob, &s_willchase1);
        ob->speed = 2048;
        break;
    case deathobj:
        sd_play_sound(KNIGHTSIGHTSND);
        new_state(ob, &s_deathchase1);
        ob->speed = 2048;
        break;
    }

    if (ob->distance < 0)
        ob->distance = 0;                   /* ignore the door opening command */

    ob->flags |= FL_ATTACKMODE | FL_FIRSTATTACK;
}

/* Called by actors that are not chasing: notice the player by sight, noise
 * or proximity, with a random reaction delay. */
static bool sight_player(objtype *ob, int tics)
{
    if (ob->flags & FL_ATTACKMODE)
        return false;                       /* DOS quits here */

    if (ob->temp2) {
        ob->temp2 -= tics;
        if (ob->temp2 > 0)
            return false;
        ob->temp2 = 0;                      /* time to react */
    } else {
        if (!areabyplayer[ob->areanumber])
            return false;

        if (ob->flags & FL_AMBUSH) {
            if (!check_sight(ob))
                return false;
            ob->flags &= ~FL_AMBUSH;
        } else {
            if (!madenoise && !check_sight(ob))
                return false;
        }

        switch (ob->obclass) {
        case guardobj:   ob->temp2 = 1 + us_rndt() / 4; break;
        case officerobj: ob->temp2 = 2;                 break;
        case mutantobj:  ob->temp2 = 1 + us_rndt() / 6; break;
        case ssobj:      ob->temp2 = 1 + us_rndt() / 6; break;
        case dogobj:     ob->temp2 = 1 + us_rndt() / 8; break;
        case bossobj: case schabbobj: case fakeobj: case mechahitlerobj:
        case realhitlerobj: case gretelobj: case giftobj: case fatobj:
        case spectreobj: case angelobj: case transobj: case uberobj:
        case willobj: case deathobj:
            ob->temp2 = 1;
            break;
        }
        return false;
    }

    first_sighting(ob);
    return true;
}

/* ------------------------------------------------------------------ */
/* Damage - KillActor, DamageActor                                     */
/* ------------------------------------------------------------------ */

static void kill_actor(objtype *ob)
{
    int tilex = ob->tilex = ob->x >> TILESHIFT;     /* drop item on center */
    int tiley = ob->tiley = ob->y >> TILESHIFT;

    switch (ob->obclass) {
    case guardobj:
        give_points(100);
        new_state(ob, &s_grddie1);
        place_item_type(bo_clip2, tilex, tiley);
        break;
    case officerobj:
        give_points(400);
        new_state(ob, &s_ofcdie1);
        place_item_type(bo_clip2, tilex, tiley);
        break;
    case mutantobj:
        give_points(700);
        new_state(ob, &s_mutdie1);
        place_item_type(bo_clip2, tilex, tiley);
        break;
    case ssobj:
        give_points(500);
        new_state(ob, &s_ssdie1);
        place_item_type(gamestate.bestweapon < wp_machinegun
                        ? bo_machinegun : bo_clip2, tilex, tiley);
        break;
    case dogobj:
        give_points(200);
        new_state(ob, &s_dogdie1);
        break;
    case bossobj:
        give_points(5000);
        new_state(ob, &s_bossdie1);
        place_item_type(bo_key1, tilex, tiley);
        break;
    case gretelobj:
        give_points(5000);
        new_state(ob, &s_greteldie1);
        place_item_type(bo_key1, tilex, tiley);
        break;
    case giftobj:
        give_points(5000);
        gamestate.killx = player.x;
        gamestate.killy = player.y;
        new_state(ob, &s_giftdie1);
        break;
    case fatobj:
        give_points(5000);
        gamestate.killx = player.x;
        gamestate.killy = player.y;
        new_state(ob, &s_fatdie1);
        break;
    case schabbobj:
        give_points(5000);
        gamestate.killx = player.x;
        gamestate.killy = player.y;
        new_state(ob, &s_schabbdie1);
        A_DeathScream(ob);
        break;
    case fakeobj:
        give_points(2000);
        new_state(ob, &s_fakedie1);
        break;
    case mechahitlerobj:
        give_points(5000);
        new_state(ob, &s_mechadie1);
        break;
    case realhitlerobj:
        give_points(5000);
        gamestate.killx = player.x;
        gamestate.killy = player.y;
        new_state(ob, &s_hitlerdie1);
        A_DeathScream(ob);
        break;
    case spectreobj:
        give_points(200);
        new_state(ob, &s_spectredie1);
        break;
    case angelobj:
        give_points(5000);
        new_state(ob, &s_angeldie1);
        break;
    case transobj:
        give_points(5000);
        new_state(ob, &s_transdie0);
        place_item_type(bo_key1, tilex, tiley);
        break;
    case uberobj:
        give_points(5000);
        new_state(ob, &s_uberdie0);
        place_item_type(bo_key1, tilex, tiley);
        break;
    case willobj:
        give_points(5000);
        new_state(ob, &s_willdie1);
        place_item_type(bo_key1, tilex, tiley);
        break;
    case deathobj:
        give_points(5000);
        new_state(ob, &s_deathdie1);
        place_item_type(bo_key1, tilex, tiley);
        break;
    }

    gamestate.killcount++;
    ob->flags &= ~FL_SHOOTABLE;
    if (in_map(tilex, tiley))
        actorat[tiley][tilex] = NULL;
    ob->flags |= FL_NONMARK;
}

static void damage_actor(objtype *ob, int damage)
{
    madenoise = true;

    /* double damage if shooting a non attack mode actor */
    if (!(ob->flags & FL_ATTACKMODE))
        damage <<= 1;

    ob->hitpoints -= damage;

    if (ob->hitpoints <= 0) {
        kill_actor(ob);
        return;
    }

    if (!(ob->flags & FL_ATTACKMODE))
        first_sighting(ob);                 /* put into combat mode */

    switch (ob->obclass) {                  /* dogs only have one hit point */
    case guardobj:
        new_state(ob, (ob->hitpoints & 1) ? &s_grdpain : &s_grdpain1);
        break;
    case officerobj:
        new_state(ob, (ob->hitpoints & 1) ? &s_ofcpain : &s_ofcpain1);
        break;
    case mutantobj:
        new_state(ob, (ob->hitpoints & 1) ? &s_mutpain : &s_mutpain1);
        break;
    case ssobj:
        new_state(ob, (ob->hitpoints & 1) ? &s_sspain : &s_sspain1);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Think and action routines - WL_ACT2.C                               */
/* ------------------------------------------------------------------ */

static void T_Stand(objtype *ob)
{
    sight_player(ob, think_tics);
}

/* The walk shared by every chaser: wait for a door, walk to the next tile,
 * pick a new one there with `select`. */
static void chase_walk(objtype *ob, void (*select)(objtype *), bool doors)
{
    int32_t move = ob->speed * think_tics;

    while (move) {
        if (doors && ob->distance < 0) {
            /* waiting for a door to open */
            open_door(-ob->distance - 1);
            if (doorobjlist[-ob->distance - 1].action != dr_open)
                return;
            ob->distance = TILEGLOBAL;      /* go ahead, the door is open */
        }

        if (move < ob->distance) {
            move_obj(ob, move);
            break;
        }

        /* reached goal tile, so select another one */
        ob->x = ((fixed)ob->tilex << TILESHIFT) + TILEGLOBAL / 2;
        ob->y = ((fixed)ob->tiley << TILESHIFT) + TILEGLOBAL / 2;

        move -= ob->distance;

        select(ob);
        if (ob->dir == nodir)
            return;                         /* object is blocked in */
    }
}

static void T_Chase(objtype *ob)
{
    int dist, chance;
    bool dodge = false;

    if (gamestate.victoryflag)
        return;

    if (check_line(ob)) {                   /* got a shot at player? */
        dist = tile_dist(ob);
        if (!dist || (dist == 1 && ob->distance < 0x4000))
            chance = 300;
        else
            chance = (think_tics << 4) / dist;

        if (us_rndt() < chance) {
            switch (ob->obclass) {
            case guardobj:       new_state(ob, &s_grdshoot1);    break;
            case officerobj:     new_state(ob, &s_ofcshoot1);    break;
            case mutantobj:      new_state(ob, &s_mutshoot1);    break;
            case ssobj:          new_state(ob, &s_ssshoot1);     break;
            case bossobj:        new_state(ob, &s_bossshoot1);   break;
            case gretelobj:      new_state(ob, &s_gretelshoot1); break;
            case mechahitlerobj: new_state(ob, &s_mechashoot1);  break;
            case realhitlerobj:  new_state(ob, &s_hitlershoot1); break;
            case angelobj:       new_state(ob, &s_angelshoot1);  break;
            case transobj:       new_state(ob, &s_transshoot1);  break;
            case uberobj:        new_state(ob, &s_ubershoot1);   break;
            case willobj:        new_state(ob, &s_willshoot1);   break;
            case deathobj:       new_state(ob, &s_deathshoot1);  break;
            }
            return;
        }
        dodge = true;
    }

    if (ob->dir == nodir) {
        if (dodge) select_dodge_dir(ob);
        else       select_chase_dir(ob);
        if (ob->dir == nodir)
            return;                         /* object is blocked in */
    }

    chase_walk(ob, dodge ? select_dodge_dir : select_chase_dir, true);
}

/* T_Schabb(), T_Gift(), T_Fat() and Spear of Destiny's T_Will(): the same
 * chase, with a flat chance of attacking and, within four tiles, running
 * away rather than closing in. */
static void T_ThrowerChase(objtype *ob, const statetype *shoot)
{
    const int dist = tile_dist(ob);
    bool dodge = false;
    int32_t move;

    if (check_line(ob)) {                   /* got a shot at player? */
        if (us_rndt() < (think_tics << 3)) {
            new_state(ob, shoot);
            return;
        }
        dodge = true;
    }

    if (ob->dir == nodir) {
        if (dodge) select_dodge_dir(ob);
        else       select_chase_dir(ob);
        if (ob->dir == nodir)
            return;                         /* object is blocked in */
    }

    move = ob->speed * think_tics;
    while (move) {
        if (ob->distance < 0) {
            open_door(-ob->distance - 1);
            if (doorobjlist[-ob->distance - 1].action != dr_open)
                return;
            ob->distance = TILEGLOBAL;
        }

        if (move < ob->distance) {
            move_obj(ob, move);
            break;
        }

        ob->x = ((fixed)ob->tilex << TILESHIFT) + TILEGLOBAL / 2;
        ob->y = ((fixed)ob->tiley << TILESHIFT) + TILEGLOBAL / 2;
        move -= ob->distance;

        if (dist < 4)
            select_run_dir(ob);
        else if (dodge)
            select_dodge_dir(ob);
        else
            select_chase_dir(ob);

        if (ob->dir == nodir)
            return;                         /* object is blocked in */
    }
}

static void T_Schabb(objtype *ob) { T_ThrowerChase(ob, &s_schabbshoot1); }
static void T_Gift(objtype *ob)   { T_ThrowerChase(ob, &s_giftshoot1); }
static void T_Fat(objtype *ob)    { T_ThrowerChase(ob, &s_fatshoot1); }

static void T_Will(objtype *ob)
{
    T_ThrowerChase(ob, ob->obclass == willobj  ? &s_willshoot1
                     : ob->obclass == angelobj ? &s_angelshoot1
                                               : &s_deathshoot1);
}

/* T_Fake(): the fake Hitler dodges about and breathes fire, and has no use
 * for doors. */
static void T_Fake(objtype *ob)
{
    if (check_line(ob)) {                   /* got a shot at player? */
        if (us_rndt() < (think_tics << 1)) {
            new_state(ob, &s_fakeshoot1);
            return;
        }
    }

    if (ob->dir == nodir) {
        select_dodge_dir(ob);
        if (ob->dir == nodir)
            return;                         /* object is blocked in */
    }

    chase_walk(ob, select_dodge_dir, false);
}

/* T_Ghosts(): straight at the player, through nothing. */
static void T_Ghosts(objtype *ob)
{
    if (ob->dir == nodir) {
        select_chase_dir(ob);
        if (ob->dir == nodir)
            return;                         /* object is blocked in */
    }

    chase_walk(ob, select_chase_dir, false);
}

static void T_DogChase(objtype *ob)
{
    int32_t move, dx, dy;

    if (ob->dir == nodir) {
        select_dodge_dir(ob);
        if (ob->dir == nodir)
            return;
    }

    move = ob->speed * think_tics;

    while (move) {
        /* check for bite range */
        dx = player.x - ob->x;
        if (dx < 0) dx = -dx;
        dx -= move;
        if (dx <= MINACTORDIST) {
            dy = player.y - ob->y;
            if (dy < 0) dy = -dy;
            dy -= move;
            if (dy <= MINACTORDIST) {
                new_state(ob, &s_dogjump1);
                return;
            }
        }

        if (move < ob->distance) {
            move_obj(ob, move);
            break;
        }

        ob->x = ((fixed)ob->tilex << TILESHIFT) + TILEGLOBAL / 2;
        ob->y = ((fixed)ob->tiley << TILESHIFT) + TILEGLOBAL / 2;

        move -= ob->distance;

        select_dodge_dir(ob);
        if (ob->dir == nodir)
            return;
    }
}

/* SelectPathDir: arrows on the object plane turn a patrol. */
static void select_path_dir(objtype *ob)
{
    unsigned spot = in_map(ob->tilex, ob->tiley)
                  ? (unsigned)plane1[ob->tiley][ob->tilex] - ICONARROWS : 99;

    if (spot < 8)
        ob->dir = spot;

    ob->distance = TILEGLOBAL;
    if (!try_walk(ob))
        ob->dir = nodir;
}

static void T_Path(objtype *ob)
{
    int32_t move;

    if (sight_player(ob, think_tics))
        return;

    if (ob->dir == nodir) {
        select_path_dir(ob);
        if (ob->dir == nodir)
            return;                         /* all movement is blocked */
    }

    move = ob->speed * think_tics;

    while (move) {
        if (ob->distance < 0) {
            open_door(-ob->distance - 1);
            if (doorobjlist[-ob->distance - 1].action != dr_open)
                return;
            ob->distance = TILEGLOBAL;
        }

        if (move < ob->distance) {
            move_obj(ob, move);
            break;
        }

        ob->x = ((fixed)ob->tilex << TILESHIFT) + TILEGLOBAL / 2;
        ob->y = ((fixed)ob->tiley << TILESHIFT) + TILEGLOBAL / 2;
        move -= ob->distance;

        select_path_dir(ob);
        if (ob->dir == nodir)
            return;
    }
}

/* Try to hit the player, based on range and how fast they are moving. */
static void T_Shoot(objtype *ob)
{
    int dist, hitchance, damage;

    if (!areabyplayer[ob->areanumber])
        return;
    if (!check_line(ob))                    /* player is behind a wall */
        return;

    dist = tile_dist(ob);

    if (ob->obclass == ssobj || ob->obclass == bossobj)
        dist = dist * 2 / 3;                /* ss are better shots */

    if (thrustspeed >= RUNSPEED)
        hitchance = (ob->flags & FL_VISABLE) ? 160 - dist * 16 : 160 - dist * 8;
    else
        hitchance = (ob->flags & FL_VISABLE) ? 256 - dist * 16 : 256 - dist * 8;

    if (us_rndt() < hitchance) {
        if (dist < 2)      damage = us_rndt() >> 2;
        else if (dist < 4) damage = us_rndt() >> 3;
        else               damage = us_rndt() >> 4;
        take_damage(damage, ob);
    }

    switch (ob->obclass) {
    case ssobj:
        play_sound_loc_actor(SSFIRESND, ob);
        break;
    case giftobj: case fatobj:
        play_sound_loc_actor(MISSILEFIRESND, ob);
        break;
    case mechahitlerobj: case realhitlerobj: case bossobj:
        play_sound_loc_actor(BOSSFIRESND, ob);
        break;
    case schabbobj:
        play_sound_loc_actor(SCHABBSTHROWSND, ob);
        break;
    case fakeobj:
        play_sound_loc_actor(FLAMETHROWERSND, ob);
        break;
    default:
        play_sound_loc_actor(NAZIFIRESND, ob);
        break;
    }
}

/* T_UShoot(): the Ubermutant's shot, and ten more points at close range. */
static void T_UShoot(objtype *ob)
{
    T_Shoot(ob);
    if (tile_dist(ob) <= 1)
        take_damage(10, ob);
}

static void T_Bite(objtype *ob)
{
    int32_t dx, dy;

    play_sound_loc_actor(DOGATTACKSND, ob);

    dx = player.x - ob->x;
    if (dx < 0) dx = -dx;
    dx -= TILEGLOBAL;
    if (dx <= MINACTORDIST) {
        dy = player.y - ob->y;
        if (dy < 0) dy = -dy;
        dy -= TILEGLOBAL;
        if (dy <= MINACTORDIST && us_rndt() < 180)
            take_damage(us_rndt() >> 4, ob);
    }
}

static void A_DeathScream(objtype *ob)
{
    /* The registered games give a soldier on a secret floor a one in 256
     * chance of the sixth scream (Spear of Destiny's two secret floors are
     * 19 and 20). The shareware (UPLOAD) build leaves this out. */
    if (wolf_version != WV_SHAREWARE) {
        const int mapon = level_mapon(current_level);
        const bool secret = wolf_version == WV_SPEAR ? mapon == 18 || mapon == 19
                                                     : mapon == 9;
        if (secret && !us_rndt()) {
            switch (ob->obclass) {
            case mutantobj: case guardobj: case officerobj: case ssobj:
            case dogobj:
                play_sound_loc_actor(DEATHSCREAM6SND, ob);
                return;
            }
        }
    }

    switch (ob->obclass) {
    case mutantobj:
        play_sound_loc_actor(AHHHGSND, ob);
        break;
    case guardobj: {
        static const int sounds[8] = {
            DEATHSCREAM1SND, DEATHSCREAM2SND, DEATHSCREAM3SND, DEATHSCREAM4SND,
            DEATHSCREAM5SND, DEATHSCREAM7SND, DEATHSCREAM8SND, DEATHSCREAM9SND
        };
        /* The upload build picks from the two screams it has recordings
         * for; the shareware data is that build's. */
        play_sound_loc_actor(sounds[us_rndt() % (wolf_version == WV_SHAREWARE ? 2 : 8)], ob);
        break;
    }
    case officerobj:     play_sound_loc_actor(NEINSOVASSND, ob); break;
    case ssobj:          play_sound_loc_actor(LEBENSND, ob);     break;
    case dogobj:         play_sound_loc_actor(DOGDEATHSND, ob);  break;
    case bossobj:        sd_play_sound(MUTTISND);        break;
    case schabbobj:      sd_play_sound(MEINGOTTSND);     break;
    case fakeobj:        sd_play_sound(HITLERHASND);     break;
    case mechahitlerobj: sd_play_sound(SCHEISTSND);      break;
    case realhitlerobj:  sd_play_sound(EVASND);          break;
    case gretelobj:      sd_play_sound(MEINSND);         break;
    case giftobj:        sd_play_sound(DONNERSND);       break;
    case fatobj:         sd_play_sound(ROSESND);         break;
    case spectreobj:     sd_play_sound(GHOSTFADESND);    break;
    case angelobj:       sd_play_sound(ANGELDEATHSND);   break;
    case transobj:       sd_play_sound(TRANSDEATHSND);   break;
    case uberobj:        sd_play_sound(UBERDEATHSND);    break;
    case willobj:        sd_play_sound(WILHELMDEATHSND); break;
    case deathobj:       sd_play_sound(KNIGHTDEATHSND);  break;
    }
}

static void A_Slurpie(objtype *ob)
{
    (void)ob;
    sd_play_sound(SLURPIESND);
}

static void A_Breathing(objtype *ob)
{
    (void)ob;
    sd_play_sound(ANGELTIREDSND);
}

static void A_MechaSound(objtype *ob)
{
    if (areabyplayer[ob->areanumber])
        play_sound_loc_actor(MECHSTEPSND, ob);
}

/* A_HitlerMorph(): out of the wreck of the suit steps Hitler himself. */
static void A_HitlerMorph(objtype *ob)
{
    static const int hitpoints[4] = { 500, 700, 800, 900 };
    objtype *nw = spawn_new_obj(ob->tilex, ob->tiley, &s_hitlerchase1);

    if (!nw)
        return;
    nw->speed     = SPDPATROL * 5;
    nw->x         = ob->x;
    nw->y         = ob->y;
    nw->distance  = ob->distance;
    nw->dir       = ob->dir;
    nw->flags     = ob->flags | FL_SHOOTABLE;
    nw->obclass   = realhitlerobj;
    nw->hitpoints = hitpoints[gamestate.difficulty & 3];
}

/* The Angel of Death: three sparks in a row, then she has to catch her
 * breath. */
static void A_StartAttack(objtype *ob)
{
    ob->temp1 = 0;
}

static void A_Relaunch(objtype *ob)
{
    if (++ob->temp1 == 3) {
        new_state(ob, &s_angeltired);
        return;
    }
    if (us_rndt() & 1)
        new_state(ob, &s_angelchase1);
}

static void A_Victory(objtype *ob)
{
    (void)ob;
    victory_done = true;                    /* playstate = ex_victorious */
}

/* A_Dormant(): a spectre that has faded rises again once nothing stands
 * where it lies - the player included. */
static void A_Dormant(objtype *ob)
{
    int32_t deltax = ob->x - player.x, deltay = ob->y - player.y;
    int xl, xh, yl, yh;

    if (deltax >= -MINACTORDIST && deltax <= MINACTORDIST
        && deltay >= -MINACTORDIST && deltay <= MINACTORDIST)
        return;

    xl = (ob->x - MINDIST) >> TILESHIFT;
    xh = (ob->x + MINDIST) >> TILESHIFT;
    yl = (ob->y - MINDIST) >> TILESHIFT;
    yh = (ob->y + MINDIST) >> TILESHIFT;

    for (int y = yl; y <= yh; y++)
        for (int x = xl; x <= xh; x++) {
            if (!in_map(x, y) || blockmap[y][x])
                return;
            if (actorat[y][x] && (actorat[y][x]->flags & FL_SHOOTABLE))
                return;
        }

    ob->flags |= FL_AMBUSH | FL_SHOOTABLE;
    ob->flags &= ~FL_ATTACKMODE;
    ob->dir = nodir;
    new_state(ob, &s_spectrewait1);
}

/* ------------------------------------------------------------------ */
/* Projectiles - T_Projectile, A_Smoke and the throws                  */
/* ------------------------------------------------------------------ */

/* GetNewActor() for a projectile, where `ob` is: moving at 0x2000 (the
 * flames 0x1200) along the angle to the player, never in actorat but where
 * nothing else is. */
static objtype *launch(objtype *ob, const statetype *state, int obclass,
                       int angle, int32_t speed, int flags)
{
    objtype *nw = get_new_actor();

    if (!nw)
        return NULL;
    nw->state    = state;
    nw->ticcount = 1;
    nw->tilex    = ob->tilex;
    nw->tiley    = ob->tiley;
    nw->x        = ob->x;
    nw->y        = ob->y;
    nw->obclass  = obclass;
    nw->dir      = nodir;
    nw->angle    = angle;
    nw->speed    = speed;
    nw->flags    = flags;
    nw->active   = true;
    return nw;
}

static void T_SchabbThrow(objtype *ob)
{
    objtype *nw = launch(ob, &s_needle1, needleobj, angle_to_player(ob),
                         0x2000, FL_NONMARK);
    if (nw)
        play_sound_loc_actor(SCHABBSTHROWSND, nw);
}

static void T_GiftThrow(objtype *ob)
{
    objtype *nw = launch(ob, &s_rocket, rocketobj, angle_to_player(ob),
                         0x2000, FL_NONMARK);
    if (nw)
        play_sound_loc_actor(MISSILEFIRESND, nw);
}

static void T_FakeFire(objtype *ob)
{
    objtype *nw = launch(ob, &s_fire1, fireobj, angle_to_player(ob),
                         0x1200, FL_NEVERMARK);
    if (nw)
        play_sound_loc_actor(FLAMETHROWERSND, nw);
}

/* T_Launch(): Barnacle Wilhelm's rocket, the Angel's spark, and the Death
 * Knight's pair - a shot, and a rocket four degrees to one side. */
static void T_Launch(objtype *ob)
{
    int iangle = angle_to_player(ob);
    objtype *nw;

    if (ob->obclass == deathobj) {
        T_Shoot(ob);
        if (ob->state == &s_deathshoot2) {
            iangle -= 4;
            if (iangle < 0)
                iangle += ANGLES;
        } else {
            iangle += 4;
            if (iangle >= ANGLES)
                iangle -= ANGLES;
        }
    }

    switch (ob->obclass) {
    case deathobj:
        nw = launch(ob, &s_hrocket, hrocketobj, iangle, 0x2000, FL_NONMARK);
        if (nw)
            play_sound_loc_actor(KNIGHTMISSILESND, nw);
        break;
    case angelobj:
        nw = launch(ob, &s_spark1, sparkobj, iangle, 0x2000, FL_NONMARK);
        if (nw)
            play_sound_loc_actor(ANGELFIRESND, nw);
        break;
    default:
        nw = launch(ob, &s_rocket, rocketobj, iangle, 0x2000, FL_NONMARK);
        if (nw)
            play_sound_loc_actor(MISSILEFIRESND, nw);
        break;
    }
}

/* A_Smoke(): a puff left behind a rocket every three tics. */
static void A_Smoke(objtype *ob)
{
    objtype *nw = get_new_actor();

    if (!nw)
        return;
    nw->state    = ob->obclass == hrocketobj ? &s_hsmoke1 : &s_smoke1;
    nw->ticcount = 6;
    nw->tilex    = ob->tilex;
    nw->tiley    = ob->tiley;
    nw->x        = ob->x;
    nw->y        = ob->y;
    nw->obclass  = inertobj;
    nw->active   = true;
    nw->flags    = FL_NEVERMARK;
}

/* ProjectileTryMove(): anything solid within an eighth of a tile stops it */
static bool projectile_try_move(objtype *ob)
{
    const int xl = (ob->x - PROJSIZE) >> TILESHIFT;
    const int yl = (ob->y - PROJSIZE) >> TILESHIFT;
    const int xh = (ob->x + PROJSIZE) >> TILESHIFT;
    const int yh = (ob->y + PROJSIZE) >> TILESHIFT;

    for (int y = yl; y <= yh; y++)
        for (int x = xl; x <= xh; x++)
            if (!in_map(x, y) || blockmap[y][x])
                return false;
    return true;
}

/* FixedMul(), as Wolf4SDL has T_Projectile() multiply: rounded */
static fixed fixed_mul_round(fixed a, fixed b)
{
    return (fixed)(((int64_t)a * b + 0x8000) >> 16);
}

static void T_Projectile(objtype *ob)
{
    const int32_t speed = ob->speed * think_tics;
    int32_t deltax, deltay;
    int damage = 0;

    deltax =  fixed_mul_round(speed, dos_sintable[ob->angle + 90]);
    deltay = -fixed_mul_round(speed, dos_sintable[ob->angle]);

    if (deltax > 0x10000L)
        deltax = 0x10000L;
    if (deltay > 0x10000L)
        deltay = 0x10000L;

    ob->x += deltax;
    ob->y += deltay;

    deltax = labs(ob->x - player.x);
    deltay = labs(ob->y - player.y);

    if (!projectile_try_move(ob)) {
        if (ob->obclass == rocketobj) {
            play_sound_loc_actor(MISSILEHITSND, ob);
            ob->state = &s_boom1;
        } else if (ob->obclass == hrocketobj) {
            play_sound_loc_actor(MISSILEHITSND, ob);
            ob->state = &s_hboom1;
        } else
            ob->state = NULL;               /* mark for removal */
        return;
    }

    if (deltax < PROJECTILESIZE && deltay < PROJECTILESIZE) {
        /* hit the player */
        switch (ob->obclass) {
        case needleobj:
            damage = (us_rndt() >> 3) + 20;
            break;
        case rocketobj: case hrocketobj: case sparkobj:
            damage = (us_rndt() >> 3) + 30;
            break;
        case fireobj:
            damage = us_rndt() >> 3;
            break;
        }
        take_damage(damage, ob);
        ob->state = NULL;                   /* mark for removal */
        return;
    }

    ob->tilex = ob->x >> TILESHIFT;
    ob->tiley = ob->y >> TILESHIFT;
}

/* ------------------------------------------------------------------ */
/* BJ victory - WL_ACT2.C                                              */
/* ------------------------------------------------------------------ */

/* SpawnBJVictory(). The tile is the one south of the player, but the
 * position is the player's own, and distance starts at zero - so the first
 * think snaps BJ back to that tile's center and walks him north from there,
 * through the exit tile, while VictorySpin() backs the camera away. */
void victory_tile(void)
{
    objtype *ob = spawn_new_obj(player.tilex, player.tiley + 1, &s_bjrun1);

    gamestate.victoryflag = true;
    if (!ob)
        return;
    ob->x       = player.x;
    ob->y       = player.y;
    ob->obclass = bjobj;
    ob->dir     = north;
    ob->temp2   = 6;                        /* DOS temp1: tiles to run   */
}

static void T_BJRun(objtype *ob)
{
    int32_t move = BJRUNSPEED * think_tics;

    while (move) {
        if (move < ob->distance) {
            move_obj(ob, move);
            break;
        }

        ob->x = ((fixed)ob->tilex << TILESHIFT) + TILEGLOBAL / 2;
        ob->y = ((fixed)ob->tiley << TILESHIFT) + TILEGLOBAL / 2;
        move -= ob->distance;

        select_path_dir(ob);

        if (!--ob->temp2) {
            new_state(ob, &s_bjjump1);
            return;
        }
    }
}

static void T_BJJump(objtype *ob)
{
    move_obj(ob, BJJUMPSPEED * think_tics);
}

static void T_BJYell(objtype *ob)
{
    play_sound_loc_actor(YEAHSND, ob);
}

static void T_BJDone(objtype *ob)
{
    (void)ob;
    victory_done = true;                    /* playstate = ex_victorious */
}

/* ------------------------------------------------------------------ */
/* The death cam - A_StartDeathCam                                     */
/* ------------------------------------------------------------------ */

/* CheckPosition(): is the player's square clear of anything solid? */
static bool check_position(fixed x, fixed y)
{
    const int xl = (x - PLAYERSIZE) >> TILESHIFT;
    const int yl = (y - PLAYERSIZE) >> TILESHIFT;
    const int xh = (x + PLAYERSIZE) >> TILESHIFT;
    const int yh = (y + PLAYERSIZE) >> TILESHIFT;

    for (int ty = yl; ty <= yh; ty++)
        for (int tx = xl; tx <= xh; tx++)
            if (!in_map(tx, ty) || blockmap[ty][tx])
                return false;
    return true;
}

/* A boss with a death cam has finished dying. The first time, the player
 * is put back where they fired the last shot from, turned to face the
 * boss and backed off until clear of walls, and the boss's death plays
 * again; when that is over too, the game is won. The DOS function also
 * waited, fizzled the view to "Let's see that again!" and waited again;
 * here that is game.c's, shown before the next frame is drawn. */
static void A_StartDeathCam(objtype *ob)
{
    fixed dist, x, y;
    float fangle;
    int angle;

    if (gamestate.victoryflag) {
        victory_done = true;                /* exit castle tile */
        return;
    }

    gamestate.victoryflag = true;
    deathcam_started = deathcam = true;

    /* line the angle up exactly */
    player.x = gamestate.killx;
    player.y = gamestate.killy;
    fangle = (float)atan2((float)(player.y - ob->y), (float)(ob->x - player.x));
    if (fangle < 0)
        fangle = (float)(M_PI * 2 + fangle);
    angle = (int)(fangle / (M_PI * 2) * ANGLES);
    player.angle = angle * (FINEANGLES / ANGLES);
    player.anglefrac = 0;

    /* try to position as close as possible without being in a wall */
    dist = 0x14000L;
    do {
        const fixed xmove =  fixed_by_frac(dist, dos_sintable[angle + 90]);
        const fixed ymove = -fixed_by_frac(dist, dos_sintable[angle]);
        x = ob->x - xmove;
        y = ob->y - ymove;
        dist += 0x1000;
    } while (!check_position(x, y));
    player.x = x;
    player.y = y;
    player.tilex = x >> TILESHIFT;
    player.tiley = y >> TILESHIFT;

    switch (ob->obclass) {
    case schabbobj:     new_state(ob, &s_schabbdeathcam); break;
    case realhitlerobj: new_state(ob, &s_hitlerdeathcam); break;
    case giftobj:       new_state(ob, &s_giftdeathcam);   break;
    case fatobj:        new_state(ob, &s_fatdeathcam);    break;
    }
}

/* ------------------------------------------------------------------ */
/* DoActor - WL_PLAY.C                                                 */
/* ------------------------------------------------------------------ */

static void mark(objtype *ob)
{
    if (ob->flags & FL_NEVERMARK)
        return;
    if (!in_map(ob->tilex, ob->tiley))
        return;
    if ((ob->flags & FL_NONMARK) && actorat[ob->tiley][ob->tilex])
        return;
    actorat[ob->tiley][ob->tilex] = ob;
}

/* One actor's frame. Returns false if it has left the list. */
static bool do_actor(objtype *ob, int tics)
{
    if (!ob->active && !areabyplayer[ob->areanumber])
        return true;

    if (!(ob->flags & (FL_NONMARK | FL_NEVERMARK)) && in_map(ob->tilex, ob->tiley))
        actorat[ob->tiley][ob->tilex] = NULL;

    /* transitional object */
    if (ob->ticcount) {
        ob->ticcount -= tics;
        while (ob->ticcount <= 0) {
            if (ob->state->action) {
                ob->state->action(ob);
                if (!ob->state) {
                    remove_obj(ob);
                    return false;
                }
            }
            ob->state = ob->state->next;
            if (!ob->state) {
                remove_obj(ob);
                return false;
            }
            if (!ob->state->tictime) {
                ob->ticcount = 0;
                break;
            }
            ob->ticcount += ob->state->tictime;
        }
    }

    /* think */
    if (ob->state->think) {
        ob->state->think(ob);
        if (!ob->state) {
            remove_obj(ob);
            return false;
        }
    }
    mark(ob);
    return true;
}

void actors_think(int tics)
{
    think_tics = tics;
    deathcam_started = false;
    /* an actor spawned on the way is thought for this frame too, and one
     * that leaves takes its place in the list with it */
    for (int i = 0; i < numobjs; ) {
        if (do_actor(actor_at(i), tics))
            i++;
    }
}

/* ------------------------------------------------------------------ */
/* The player's attacks - WL_AGENT.C                                   */
/* ------------------------------------------------------------------ */

void knife_attack(void)
{
    objtype *closest = NULL;
    int32_t dist = 0x7fffffff;

    for (int i = 0; i < numobjs; i++) {
        objtype *check = actor_at(i);
        if ((check->flags & FL_SHOOTABLE) && (check->flags & FL_VISABLE)
            && abs(check->aimx - AIM_CENTER) < AIM_DELTA
            && check->transx < dist) {
            dist = check->transx;
            closest = check;
        }
    }

    if (!closest || dist > 0x18000L)
        return;                             /* missed */

    damage_actor(closest, us_rndt() >> 4);
}

void gun_attack(void)
{
    objtype *closest = NULL, *oldclosest;
    int32_t viewdist = 0x7fffffff;
    int dist, damage;

    madenoise = true;

    /* Find the nearest thing in the sights and trace a line to it. The DOS
     * loop means to fall back to the next target when a wall is in the way,
     * but viewdist is not reset between passes, so the second pass can only
     * find something nearer than the first - which cannot exist. In effect
     * a blocked nearest target means a miss. Kept as it was. */
    while (1) {
        oldclosest = closest;
        for (int i = 0; i < numobjs; i++) {
            objtype *check = actor_at(i);
            if ((check->flags & FL_SHOOTABLE) && (check->flags & FL_VISABLE)
                && abs(check->aimx - AIM_CENTER) < AIM_DELTA
                && check->transx < viewdist) {
                viewdist = check->transx;
                closest = check;
            }
        }
        if (closest == oldclosest)
            return;                         /* no more targets, all missed */
        if (check_line(closest))
            break;
    }

    dist = tile_dist(closest);

    if (dist < 2)
        damage = us_rndt() / 4;
    else if (dist < 4)
        damage = us_rndt() / 6;
    else {
        if (us_rndt() / 12 < dist)          /* missed */
            return;
        damage = us_rndt() / 6;
    }

    damage_actor(closest, damage);
}

/* ------------------------------------------------------------------ */
/* CalcRotate - WL_DRAW.C                                              */
/* ------------------------------------------------------------------ */

int actor_shape(objtype *ob)
{
    int shapenum = ob->state->shapenum;
    int viewangle, angle;

    if (!ob->state->rotate)
        return shapenum;

    /* In degrees, as the DOS angles are. dirangle[] is dir * 45; a rocket
     * turns by its own angle. */
    viewangle = player.angle / 10 + (CENTERX - ob->viewx) / 8;
    if (ob->obclass == rocketobj || ob->obclass == hrocketobj)
        angle = (viewangle - 180) - ob->angle;
    else
        angle = (viewangle - 180) - ob->dir * 45;
    angle += 360 / 16;
    while (angle >= 360) angle -= 360;
    while (angle < 0)    angle += 360;

    if (ob->state->rotate == 2)             /* 2 rotation pain frame */
        return shapenum + 4 * (angle / 180);
    return shapenum + angle / 45;
}
