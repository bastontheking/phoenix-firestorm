/**
 * @file llbuildlistener.h
 * @brief LEAP API for in-world building: prims, faces, scripts, local mesh and chat capture.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * $/LicenseInfo$
 */

#ifndef LL_LLBUILDLISTENER_H
#define LL_LLBUILDLISTENER_H

#include "lleventapi.h"

#include <boost/signals2.hpp>
#include <deque>

class LLMessageSystem;

class LLBuildListener : public LLEventAPI
{
public:
    LLBuildListener();

    // Called for every ObjectPropertiesFamily reply: caches name/desc/owner of in-world objects.
    static void processObjectPropertiesFamily(LLMessageSystem* msg);
    static LLSD cachedObjectProperties(const LLUUID& id);

    // Starts/stops firestorm-mcp/leap_bridge.py as a LEAP plugin (Help menu), so the MCP
    // server runs without launching the viewer through a separate script.
    static bool isMCPBridgeRunning();
    static void toggleMCPBridge();

private:
    // context / reading
    void getContext(LLSD const& data);
    void getObject(LLSD const& data);
    void getSelection(LLSD const& data);
    void selectObjects(LLSD const& data);

    // creation / editing
    void createPrim(LLSD const& data);
    void setTransform(LLSD const& data);
    void setShape(LLSD const& data);
    void setFaces(LLSD const& data);
    void setNameDesc(LLSD const& data);
    void setFlags(LLSD const& data);
    void linkObjects(LLSD const& data);
    void unlinkObjects(LLSD const& data);
    void deleteObjects(LLSD const& data);

    // scripts
    void getObjectInventory(LLSD const& data);
    void createScript(LLSD const& data);
    void uploadScript(LLSD const& data);
    void setScriptRunning(LLSD const& data);

    // local mesh / mesh upload
    void localMeshAdd(LLSD const& data);
    void localMeshList(LLSD const& data);
    void localMeshApply(LLSD const& data);
    void localMeshReload(LLSD const& data);
    void localMeshClear(LLSD const& data);
    void localMeshRemove(LLSD const& data);
    void openMeshUpload(LLSD const& data);

    // inventory <-> world
    void rezFromInventory(LLSD const& data);
    void takeObjects(LLSD const& data);
    void duplicateObject(LLSD const& data);

    // object contents / notecards / script text
    void addInventoryToObject(LLSD const& data);
    void removeObjectInventory(LLSD const& data);
    void createNotecard(LLSD const& data);
    void uploadNotecard(LLSD const& data);
    void readItemText(LLSD const& data);
    void resetScript(LLSD const& data);

    // costs and limits
    void getObjectCosts(LLSD const& data);
    void getParcelInfo(LLSD const& data);
    void canBuildAt(LLSD const& data);

    // materials, extra prim params, uploads
    void setRenderMaterial(LLSD const& data);
    void setExtraParams(LLSD const& data);
    void uploadFile(LLSD const& data);

    // world object names / what the avatar wears
    void requestObjectProperties(LLSD const& data);
    void getObjectProperties(LLSD const& data);
    void getWornItems(LLSD const& data);

    // avatar / camera / verification
    void getAvatarPose(LLSD const& data);
    void snapshot(LLSD const& data);
    void resetCamera(LLSD const& data);

    // chat capture
    void getChat(LLSD const& data);

    // <FS:MCP> bridge status, permissions/sale, wearing, IM, teleport, export, undo support
    void bridgeReady(LLSD const& data);
    void setPermissions(LLSD const& data);
    void setSaleInfo(LLSD const& data);
    void wearItem(LLSD const& data);
    void removeItem(LLSD const& data);
    void getIM(LLSD const& data);
    void confirmTeleport(LLSD const& data);
    void openExport(LLSD const& data);
    void setVolumeParams(LLSD const& data);

    // <FS:MCP> HUDs, touching, script dialogs
    void getHudObjects(LLSD const& data);
    void getScreenInfo(LLSD const& data);
    void touchObject(LLSD const& data);
    void touchAt(LLSD const& data);
    void getScriptDialogs(LLSD const& data);
    void answerScriptDialog(LLSD const& data);

    void ensureChatHooked();
    void onChat(const LLSD& chat);

    boost::signals2::connection mChatConnection;
    std::deque<LLSD> mChatLog;
    S32 mChatSeq = 0;
};

#endif // LL_LLBUILDLISTENER_H
