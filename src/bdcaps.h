/* Player capabilities for bdnavd.
 *
 * libbluray's default PSR31 is a profile 5 (non-UHD) player. UHD discs'
 * BD-J menus read PSR25/26/27/31 and refuse to draw on a player that does
 * not claim UHD. These are the values Kodi sets
 * (DVDInputStreamBluray.cpp, SetupPlayerSettings). */
#ifndef BDCAPS_H
#define BDCAPS_H
#include <libbluray/bluray.h>
#include <libbluray/player_settings.h>

static inline void bd_apply_player_caps(BLURAY *bd) {
  bd_set_player_setting(bd, BLURAY_PLAYER_SETTING_PARENTAL, 99);
  bd_set_player_setting(bd, BLURAY_PLAYER_SETTING_3D_CAP, 0xffffffff);
  bd_set_player_setting(bd, BLURAY_PLAYER_SETTING_PLAYER_PROFILE, BLURAY_PLAYER_PROFILE_6_v3_1);
  bd_set_player_setting(bd, BLURAY_PLAYER_SETTING_UHD_CAP, 0xffffffff);
  bd_set_player_setting(bd, BLURAY_PLAYER_SETTING_UHD_DISPLAY_CAP, 0xffffffff);
  bd_set_player_setting(bd, BLURAY_PLAYER_SETTING_HDR_PREFERENCE, 0xffffffff);
  /* Enforce the disc's user-operation masks as a player must: libbluray's default
     (relaxed) let the remote skip chapters and change angles a disc forbids. */
  bd_set_player_setting(bd, BLURAY_PLAYER_SETTING_UO_RESTRICTION_LEVEL,
                        BLURAY_PLAYER_SETTING_UO_RESTRICTION_COMPLIANT);
}
#endif
