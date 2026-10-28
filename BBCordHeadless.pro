# BBCord headless service. Keeps the Discord gateway connected while the BBCord UI is closed
# (DMs/mentions keep reaching BlackBerry Hub) and handles Hub's Mark Read / Mark Unread /
# Delete without launching the UI. It compiles the BBCord core straight from the BBCord
# project, so both always run the exact same client code.
#
# Expects <workspace>/BBCord/ (with src/ and third_party/) next to this project.
APP_NAME = BBCordHeadless

CONFIG += qt warn_on
CONFIG -= cascades10
QT += network
QT -= gui

# Folder name of the BBCord project (case matters outside Windows).
UI_ROOT = ../BBCord
UI_SRC  = $$UI_ROOT/src

# bbdata: bb::data::JsonDataAccess, bbmultimedia: HubIntegration's ping player (unused in
# service mode, still linked), bbsystem: InvokeManager, bb: bb::Application,
# bbpim + unifieddatasourcec: UDS (Hub). Same set as BBCord, minus Cascades.
LIBS += -lbbdata -lbbmultimedia -lbbsystem -lbb -lbbplatform -lbbpim -lunifieddatasourcec
LIBS += -lsocket -lm -lz

INCLUDEPATH += src $$UI_SRC $$UI_ROOT/third_party/mongoose

# Mongoose (HTTP/WebSocket + TLS), built exactly like in BBCord.pro.
SOURCES += $$UI_ROOT/third_party/mongoose/mongoose.c
HEADERS += $$UI_ROOT/third_party/mongoose/mongoose.h
DEFINES += MG_TLS=MG_TLS_OPENSSL MG_ENABLE_POLL=1 MG_ENABLE_LOG=0 \
           MG_ENABLE_OPENSSL=1 MG_MAX_RECV_SIZE=67108864 MG_ENABLE_LINES=1
QMAKE_CFLAGS += -Wc,-std=gnu99
QMAKE_CFLAGS += -Wc,"-DMG_TLS=MG_TLS_OPENSSL"
QMAKE_CFLAGS += -Wc,"-DMG_ENABLE_OPENSSL=1"
QMAKE_CFLAGS += -Wc,"-DMG_ENABLE_POLL=1"
QMAKE_CFLAGS += -Wc,"-DMG_ENABLE_LOG=0"
QMAKE_CFLAGS += -Wc,"-DMG_MAX_RECV_SIZE=67108864"

# LIBS paths are used from the build folder (arm/ or x86/), so they must be absolute: $$PWD.
simulator {
    INCLUDEPATH += $$UI_ROOT/third_party/prebuilt/simulator/openssl/include
    LIBS += $$PWD/$$UI_ROOT/third_party/prebuilt/simulator/openssl/lib/libssl.a
    LIBS += $$PWD/$$UI_ROOT/third_party/prebuilt/simulator/openssl/lib/libcrypto.a
}
device {
    INCLUDEPATH += $$UI_ROOT/third_party/prebuilt/device/openssl/include
    LIBS += $$PWD/$$UI_ROOT/third_party/prebuilt/device/openssl/lib/libssl.a
    LIBS += $$PWD/$$UI_ROOT/third_party/prebuilt/device/openssl/lib/libcrypto.a
}

HEADERS += \
    src/core/ServiceController.hpp \
    $$UI_ROOT/src/core/AppStore.hpp \
    $$UI_ROOT/src/core/AttachmentImageCacheWorker.hpp \
    $$UI_ROOT/src/core/AvatarCacheWorker.hpp \
    $$UI_ROOT/src/core/Client.hpp \
    $$UI_ROOT/src/core/HubIntegration.hpp \
    $$UI_ROOT/src/core/HubShared.hpp \
    $$UI_ROOT/src/core/ServiceHandoff.hpp \
    $$UI_ROOT/src/core/client/AvatarManager.hpp \
    $$UI_ROOT/src/core/client/AvatarState.hpp \
    $$UI_ROOT/src/core/client/CacheManager.hpp \
    $$UI_ROOT/src/core/client/ClientState.hpp \
    $$UI_ROOT/src/core/client/GatewayHandler.hpp \
    $$UI_ROOT/src/core/client/ItemMapper.hpp \
    $$UI_ROOT/src/core/client/MessageCache.hpp \
    $$UI_ROOT/src/core/client/PermissionUtils.hpp \
    $$UI_ROOT/src/core/client/SortUtils.hpp \
    $$UI_ROOT/src/core/discord/DiscordUtils.hpp \
    $$UI_ROOT/src/core/discord/Gateway.hpp \
    $$UI_ROOT/src/core/discord/GatewayWorker.hpp \
    $$UI_ROOT/src/core/discord/JsonParser.hpp \
    $$UI_ROOT/src/core/discord/NetworkWorker.hpp \
    $$UI_ROOT/src/core/discord/RestClient.hpp \
    $$UI_ROOT/src/core/models/Models.hpp \
    $$UI_ROOT/src/core/modules/Chat.hpp \
    $$UI_ROOT/src/core/modules/DM.hpp \
    $$UI_ROOT/src/core/modules/FeatureConstants.hpp \
    $$UI_ROOT/src/core/modules/GuildChannels.hpp \
    $$UI_ROOT/src/core/modules/Guilds.hpp \
    $$UI_ROOT/src/core/modules/User.hpp \
    $$UI_ROOT/src/core/modules/Worker.hpp \
    $$UI_ROOT/src/utils/EmojiUtils.hpp \
    $$UI_ROOT/src/utils/MarkdownParser.hpp

SOURCES += \
    src/main.cpp \
    src/core/ServiceController.cpp \
    $$UI_ROOT/src/core/AppStore.cpp \
    $$UI_ROOT/src/core/AttachmentImageCacheWorker.cpp \
    $$UI_ROOT/src/core/AvatarCacheWorker.cpp \
    $$UI_ROOT/src/core/Client.cpp \
    $$UI_ROOT/src/core/HubIntegration.cpp \
    $$UI_ROOT/src/core/HubShared.cpp \
    $$UI_ROOT/src/core/client/AvatarManager.cpp \
    $$UI_ROOT/src/core/client/AvatarState.cpp \
    $$UI_ROOT/src/core/client/CacheManager.cpp \
    $$UI_ROOT/src/core/client/ClientState.cpp \
    $$UI_ROOT/src/core/client/GatewayHandler.cpp \
    $$UI_ROOT/src/core/client/ItemMapper.cpp \
    $$UI_ROOT/src/core/client/MessageCache.cpp \
    $$UI_ROOT/src/core/client/PermissionUtils.cpp \
    $$UI_ROOT/src/core/client/SortUtils.cpp \
    $$UI_ROOT/src/core/discord/DiscordUtils.cpp \
    $$UI_ROOT/src/core/discord/Gateway.cpp \
    $$UI_ROOT/src/core/discord/GatewayEvents.cpp \
    $$UI_ROOT/src/core/discord/GatewayWorker.cpp \
    $$UI_ROOT/src/core/discord/JsonParser.cpp \
    $$UI_ROOT/src/core/discord/NetworkWorker.cpp \
    $$UI_ROOT/src/core/discord/RestClient.cpp \
    $$UI_ROOT/src/core/discord/RestClientRequests.cpp \
    $$UI_ROOT/src/core/discord/rest/Avatar.cpp \
    $$UI_ROOT/src/core/discord/rest/Channel.cpp \
    $$UI_ROOT/src/core/discord/rest/Guild.cpp \
    $$UI_ROOT/src/core/discord/rest/Login.cpp \
    $$UI_ROOT/src/core/discord/rest/Message.cpp \
    $$UI_ROOT/src/core/models/Models.cpp \
    $$UI_ROOT/src/core/modules/Chat.cpp \
    $$UI_ROOT/src/core/modules/DM.cpp \
    $$UI_ROOT/src/core/modules/GuildChannels.cpp \
    $$UI_ROOT/src/core/modules/Guilds.cpp \
    $$UI_ROOT/src/core/modules/User.cpp \
    $$UI_ROOT/src/core/modules/Worker.cpp \
    $$UI_ROOT/src/utils/EmojiUtils.cpp \
    $$UI_ROOT/src/utils/MarkdownParser.cpp

CONFIG -= precompile_header
CONFIG -= precompiled_header
PRECOMPILED_HEADER =
