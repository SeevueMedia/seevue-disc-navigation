/* Player capabilities for bdnavd, per session.
 *
 * The player claims Ultra HD (profile 6 v3.1 and the UHD registers PSR
 * 25 to 27, Kodi's values in DVDInputStreamBluray.cpp) only when the
 * session's client can decode Ultra HD, and an HDR display only when it can
 * show HDR: a UHD disc then shows its "needs an Ultra HD player" screen or its
 * SDR branch instead of a picture the client cannot play. Without Ultra HD it
 * is a profile 2 (BD-Live class) player. 3D is never claimed. */
#ifndef BDCAPS_H
#define BDCAPS_H
#include <stdint.h>
#include <libbluray/bluray.h>
#include <libbluray/player_settings.h>

#define BDCAP_UHD 1u
#define BDCAP_HDR 2u

static inline void bd_apply_player_caps(BLURAY *bd, uint8_t parental_age, uint8_t flags) {
  int uhd = (flags & BDCAP_UHD) != 0, hdr = uhd && (flags & BDCAP_HDR) != 0;
  bd_set_player_setting(bd, BLURAY_PLAYER_SETTING_PARENTAL, parental_age);
  bd_set_player_setting(bd, BLURAY_PLAYER_SETTING_3D_CAP, 0);
  bd_set_player_setting(bd, BLURAY_PLAYER_SETTING_PLAYER_PROFILE,
                        uhd ? BLURAY_PLAYER_PROFILE_6_v3_1 : BLURAY_PLAYER_PROFILE_2_v2_0);
  bd_set_player_setting(bd, BLURAY_PLAYER_SETTING_UHD_CAP, uhd ? 0xffffffff : 0);
  bd_set_player_setting(bd, BLURAY_PLAYER_SETTING_UHD_DISPLAY_CAP, hdr ? 0xffffffff : 0);
  bd_set_player_setting(bd, BLURAY_PLAYER_SETTING_HDR_PREFERENCE, hdr ? 0xffffffff : 0);
  /* Enforce the disc's user-operation masks as a player must: libbluray's default
     (relaxed) let the remote skip chapters and change angles a disc forbids. */
  bd_set_player_setting(bd, BLURAY_PLAYER_SETTING_UO_RESTRICTION_LEVEL,
                        BLURAY_PLAYER_SETTING_UO_RESTRICTION_COMPLIANT);
}
#endif
