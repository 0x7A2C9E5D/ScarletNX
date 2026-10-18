/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/core/hle/service/acc/acc.cpp, errors.h):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project (GPL-2.0-or-later) */
/* acc: the console's one user (g_user_id, am.c), open since the game started, with no network service account */
#include "nx/hle/services/services_int.h"
#include "gui/splash.h"

#include "hle/services/acc.swipc.inc"

#define ACC_INVALID_USER_ID RC(124, 20)
#define ACC_INVALID_LENGTH  RC(124, 32)
#define ACC_THUMBNAIL_SIZE  0x24000      /* a save's thumbnail: 256x144 RGBA */
#define ACC_NOT_CONNECTED   RC(110, 300) /* nifm's: there is no network to reach the account server */
#define ACC_NICKNAME        "Player"
#define ACC_LAST_EDIT       1546300800 /* the user's profile was last changed on the first day of 2019 */

static int acc_is_user(const nn_account_Uid* uid) { return !memcmp(uid->v, g_user_id, 16); }

/* the user, then zeros (no user) in the rest of the list */
static void acc_list(IpcBuf users) {
    memset(GP(users.addr), 0, (size_t)users.size);
    if (users.size >= 16) memcpy(GP(users.addr), g_user_id, 16);
}

/* an object that keeps the user it was made for */
static SvcObj* acc_for_user(SvcObj* o, const nn_account_Uid* uid) {
    memcpy(o->data, uid->v, 16);
    return o;
}

/* ============================================================ IAccountServiceForApplication */
static int nn_account_IAccountServiceForApplication_GetUserCount(SvcObj* self, i32* count) {
    (void)self;
    *count = 1;
    R_SUCCEED();
}
static int nn_account_IAccountServiceForApplication_GetUserExistence(SvcObj* self, const nn_account_Uid* uid,
                                                                    u8* exists) {
    (void)self;
    *exists = (u8)acc_is_user(uid);
    R_SUCCEED();
}
static int nn_account_IAccountServiceForApplication_ListAllUsers(SvcObj* self, IpcBuf users) {
    (void)self;
    acc_list(users);
    R_SUCCEED();
}
static int nn_account_IAccountServiceForApplication_ListOpenUsers(SvcObj* self, IpcBuf users) {
    (void)self;
    acc_list(users);
    R_SUCCEED();
}
static int nn_account_IAccountServiceForApplication_GetLastOpenedUser(SvcObj* self, nn_account_Uid* uid) {
    (void)self;
    memcpy(uid->v, g_user_id, 16);
    R_SUCCEED();
}
static int nn_account_IAccountServiceForApplication_GetProfile(SvcObj* self, const nn_account_Uid* uid,
                                                              SvcObj** profile) {
    (void)self;
    R_UNLESS(acc_is_user(uid), ACC_INVALID_USER_ID);
    *profile = acc_for_user(nn_account_profile_IProfile_new(16), uid);
    R_SUCCEED();
}
/* with one user, that user, chosen without asking */
static int nn_account_IAccountServiceForApplication_TrySelectUserWithoutInteractionDeprecated(
    SvcObj* self, u8 is_network_service_account_required, nn_account_Uid* uid) {
    (void)self, (void)is_network_service_account_required;
    memcpy(uid->v, g_user_id, 16);
    R_SUCCEED();
}
static int nn_account_IAccountServiceForApplication_TrySelectUserWithoutInteraction(
    SvcObj* self, u8 is_network_service_account_required, nn_account_Uid* uid) {
    return nn_account_IAccountServiceForApplication_TrySelectUserWithoutInteractionDeprecated(
        self, is_network_service_account_required, uid);
}
static int nn_account_IAccountServiceForApplication_GetBaasAccountManagerForApplication(SvcObj* self,
                                                                                       const nn_account_Uid* uid,
                                                                                       SvcObj** manager) {
    (void)self;
    R_UNLESS(acc_is_user(uid), ACC_INVALID_USER_ID);
    *manager = acc_for_user(nn_account_baas_IManagerForApplication_new(16), uid);
    R_SUCCEED();
}
static int nn_account_IAccountServiceForApplication_LoadOpenContext(SvcObj* self, const nn_account_Uid* uid,
                                                                   SvcObj** manager) {
    return nn_account_IAccountServiceForApplication_GetBaasAccountManagerForApplication(self, uid, manager);
}
static int nn_account_IAccountServiceForApplication_ListOpenContextStoredUsers(SvcObj* self, IpcBuf users) {
    (void)self;
    acc_list(users);
    R_SUCCEED();
}
static int nn_account_IAccountServiceForApplication_ListQualifiedUsers(SvcObj* self, IpcBuf users) {
    (void)self;
    acc_list(users);
    R_SUCCEED();
}

/* ============================================================ IProfile */
/* the thumbnail a game stores with a save: for a user, at its size. None is kept here, the saves have no screen to
   show it on */
static int nn_account_IAccountServiceForApplication_StoreSaveDataThumbnail(SvcObj* self, const nn_account_Uid* uid,
                                                                           IpcBuf image) {
    static const nn_account_Uid none;
    (void)self;
    R_UNLESS(memcmp(uid->v, none.v, sizeof none.v), ACC_INVALID_USER_ID);
    R_UNLESS(image.size == ACC_THUMBNAIL_SIZE, ACC_INVALID_LENGTH);
    R_SUCCEED();
}

static void acc_profile_base(SvcObj* self, nn_account_profile_ProfileBase* base) {
    memcpy(base->uid.v, self->data, 16);
    base->last_edit_timestamp = ACC_LAST_EDIT;
    memcpy(base->nickname, ACC_NICKNAME, sizeof ACC_NICKNAME);
}
/* the user data (icon, colors) is all zeros: the default icon */
static int nn_account_profile_IProfile_Get(SvcObj* self, IpcBuf user_data, nn_account_profile_ProfileBase* base) {
    memset(GP(user_data.addr), 0, (size_t)user_data.size);
    acc_profile_base(self, base);
    R_SUCCEED();
}
static int nn_account_profile_IProfile_GetBase(SvcObj* self, nn_account_profile_ProfileBase* base) {
    acc_profile_base(self, base);
    R_SUCCEED();
}

/* the user's picture: Wimsy in Fuddles' colours (dist/splash/wimsy-fuddles.svg, the splash's drawing) at 256x256 over
   white, a JPEG as the console's pictures are, drawn at the first ask and kept */
#define SNX_ACC_PICTURE_SIDE    256
#define SNX_ACC_PICTURE_QUALITY 90
#define SNX_ACC_PICTURE_MAX     0x20000 /* the most a game takes */
static struct {
    PMutex lk;
    u32 size;
    int made, overflow; /* made: drawn or tried; overflow: more than SNX_ACC_PICTURE_MAX came */
    u8 d[SNX_ACC_PICTURE_MAX];
} acc_pic = {PMUTEX_INIT, 0, 0, 0, {0}};

static void acc_picture_write(void* closure, void* data, int size) {
    (void)closure;
    if (size < 0 || (u32)size > SNX_ACC_PICTURE_MAX - acc_pic.size) {
        acc_pic.overflow = 1;
        return;
    }
    memcpy(acc_pic.d + acc_pic.size, data, (size_t)size);
    acc_pic.size += (u32)size;
}
/* the picture's size, drawing it the first time; 0, said once, when it cannot be drawn */
static u32 acc_picture(void) {
    u32 size;
    pm_lock(&acc_pic.lk);
    if (!acc_pic.made) {
        acc_pic.made = 1;
        if (snx_splash_jpeg("wimsy-fuddles", SNX_ACC_PICTURE_SIDE, SNX_ACC_PICTURE_QUALITY, acc_picture_write, NULL) ||
            acc_pic.overflow) {
            WARN("acc: the user's picture was not drawn, games get none\n");
            acc_pic.size = 0;
        }
    }
    size = acc_pic.size;
    pm_unlock(&acc_pic.lk);
    return size;
}
/* GetImageSize tells the size LoadImage writes, so a game reserves what it gets */
static int nn_account_profile_IProfile_GetImageSize(SvcObj* self, u32* size) {
    (void)self;
    *size = acc_picture();
    R_SUCCEED();
}
static int nn_account_profile_IProfile_LoadImage(SvcObj* self, IpcBuf image, u32* size) {
    (void)self;
    *size = acc_picture();
    memcpy(GP(image.addr), acc_pic.d, (size_t)(image.size < *size ? image.size : *size));
    R_SUCCEED();
}

/* ============================================================ IManagerForApplication */
/* the id the account would have, made from the user's */
static int nn_account_baas_IManagerForApplication_GetAccountId(SvcObj* self, nn_account_NetworkServiceAccountId* id) {
    u64 a, b;
    memcpy(&a, self->data, 8);
    memcpy(&b, (u8*)self->data + 8, 8);
    *id = a ^ b;
    R_SUCCEED();
}
static int nn_account_baas_IManagerForApplication_EnsureIdTokenCacheAsync(SvcObj* self, SvcObj** context) {
    (void)self;
    *context = nn_account_detail_IAsyncContext_new(0);
    R_SUCCEED();
}

/* ============================================================ IAsyncContext: done at once, failed */
static int nn_account_detail_IAsyncContext_GetSystemEvent(SvcObj* self, u32* event) {
    (void)self;
    return new_event(event, "acc async context", 1);
}
static int nn_account_detail_IAsyncContext_HasDone(SvcObj* self, u8* done) {
    (void)self;
    *done = 1;
    R_SUCCEED();
}
static int nn_account_detail_IAsyncContext_GetResult(SvcObj* self) {
    (void)self;
    R_THROW(ACC_NOT_CONNECTED);
}

SvcObj* new_acc(void) { return nn_account_IAccountServiceForApplication_new(0); }
