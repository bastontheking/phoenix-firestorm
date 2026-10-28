/**
 * @file llbuildlistener.cpp
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

#include "llviewerprecompiledheaders.h"

#include "llbuildlistener.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llagentbenefits.h"
#include "llagentwearables.h"
#include "llassetstorage.h"
#include "llavatarnamecache.h"
#include "lldir.h"
#include "llfilesystem.h"
#include "llmemorystream.h"
#include "llnotecard.h"
#include "llparcel.h"
#include "llviewermenufile.h"
#include "llviewerparcelmgr.h"
#include "llviewerparceloverlay.h"
#include "llvovolume.h"
#include "llwearabletype.h"
#include "llcallbacklist.h"
#include "llfloaterimnearbychathandler.h"
#include "llfloatermodelpreview.h"
#include "llfloaterperms.h"
#include "llfloaterreg.h"
#include "llinventorydefines.h"
#include "llinventorymodel.h"
#include "llnotificationmanager.h"
#include "llsdutil_math.h"
#include "llselectmgr.h"
#include "llviewerassetupload.h"
#include "llviewerinventory.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llviewerwindow.h"
#include "llvoavatarself.h"
#include "llvoinventorylistener.h"
#include "llviewerassettype.h"
#include "llvolumemessage.h"
#include "llworld.h"
#include "fscommon.h"
#include "stringize.h"
#include "vjlocalmesh.h"
#include "llleap.h"
#include "lltoolgrab.h"
#include "llscriptruntimeperms.h"
#include "lllslconstants.h"
#include "llnotifications.h"
#include "llappearancemgr.h"
#include "llimview.h"
#include "llinventorybridge.h"
#include "llinventoryfunctions.h"
#include "llsaleinfo.h"
#include "llviewerjointattachment.h"
#include <csignal>
#include "llnotificationsutil.h"
#include "llviewercontrol.h"

// The only instance; LEAP plugins reach it through the "LLBuild" pump.
static LLBuildListener sBuildListener;

// ---- MCP bridge (LEAP plugin) started from the Help menu -------------------

namespace
{
    LLLeap* sMCPBridge = nullptr;    // bridge we started; validated against the LLLeap instances
    S32 sBridgePid = 0;              // reported by the bridge (bridgeReady), also for --leap starts
    S32 sBridgePort = 0;
    bool sBridgeReady = false;
    bool sBridgeStopping = false;    // user asked to stop: no "stopped unexpectedly" notice
    bool sBridgeWatching = false;

    LLLeap* live_mcp_bridge()
    {
        if (!sMCPBridge)
            return nullptr;
        for (auto& leap : LLLeap::instance_snapshot())
        {
            if (&leap == sMCPBridge)
                return sMCPBridge;
        }
        sMCPBridge = nullptr; // child exited, LLLeap deleted itself
        return nullptr;
    }

    bool pid_alive(S32 pid)
    {
        if (pid <= 0)
            return false;
#if LL_WINDOWS
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
        if (!process)
            return false;
        DWORD code = 0;
        bool alive = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
        CloseHandle(process);
        return alive;
#else
        return ::kill((pid_t)pid, 0) == 0;
#endif
    }

    void kill_pid(S32 pid)
    {
#if LL_WINDOWS
        if (HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)pid))
        {
            TerminateProcess(process, 0);
            CloseHandle(process);
        }
#else
        ::kill((pid_t)pid, SIGTERM);
#endif
    }

    bool bridge_running()
    {
        return live_mcp_bridge() || pid_alive(sBridgePid);
    }

    // FSMCPBridgeDir, else $FIRESTORM_MCP_DIR, else ~/Projetos/firestorm-mcp
    std::string mcp_bridge_dir()
    {
        std::string dir = gSavedSettings.getString("FSMCPBridgeDir");
        if (dir.empty())
        {
            if (const char* env = getenv("FIRESTORM_MCP_DIR"))
                dir = env;
        }
        if (dir.empty())
        {
#if LL_WINDOWS
            const char* home = getenv("USERPROFILE");
#else
            const char* home = getenv("HOME");
#endif
            dir = gDirUtilp->add(home ? home : "", "Projetos", "firestorm-mcp");
        }
        return dir;
    }

    void mcp_notify(const std::string& message)
    {
        LLNotificationsUtil::add("SystemMessageTip", LLSD().with("MESSAGE", message));
    }

    void reset_bridge_state()
    {
        sMCPBridge = nullptr;
        sBridgePid = 0;
        sBridgePort = 0;
        sBridgeReady = false;
    }

    // Once per second while a bridge is (or should be) running: tells the user when it dies
    // on its own, e.g. because the port is taken or Python is missing a module.
    void watch_bridge()
    {
        if (sBridgeWatching)
            return;
        sBridgeWatching = true;
        doPeriodically([]()
        {
            if (bridge_running())
                return false;
            if (!sBridgeStopping)
            {
                mcp_notify(sBridgeReady
                    ? std::string("Servidor MCP parou inesperadamente (veja o log do viewer).")
                    : std::string("Servidor MCP nao conseguiu iniciar: porta ocupada, Python ou modulos faltando? Veja o log do viewer."));
            }
            reset_bridge_state();
            sBridgeStopping = false;
            sBridgeWatching = false;
            return true;
        }, 1.f);
    }
}

//static
bool LLBuildListener::isMCPBridgeRunning()
{
    return bridge_running();
}

//static
void LLBuildListener::toggleMCPBridge()
{
    if (bridge_running())
    {
        sBridgeStopping = true;
        if (LLLeap* bridge = live_mcp_bridge())
            bridge->stop();
        else
            kill_pid(sBridgePid); // started with --leap, not by us
        mcp_notify("Servidor MCP desligado.");
        return;
    }

    std::string dir = mcp_bridge_dir();
    std::string script = gDirUtilp->add(dir, "leap_bridge.py");
    if (!LLFile::isfile(script))
    {
        mcp_notify("Servidor MCP: nao encontrei " + script +
                   ". Ajuste FSMCPBridgeDir nas Debug Settings.");
        return;
    }

    // Prefer the bridge's own venv (has mcp + llsd), else the system interpreter.
#if LL_WINDOWS
    std::string python = gDirUtilp->add(dir, ".venv", "Scripts", "python.exe");
    if (!LLFile::isfile(python))
        python = "python";
#else
    std::string python = gDirUtilp->add(dir, ".venv", "bin", "python");
    if (!LLFile::isfile(python))
        python = "python3";
#endif

    reset_bridge_state();
    sBridgeStopping = false;
    sMCPBridge = LLLeap::create("firestorm-mcp", std::vector<std::string>{ python, script }, false);
    if (sMCPBridge)
    {
        // The "ligado" notice comes from bridgeReady, once the HTTP port is really listening.
        mcp_notify("Servidor MCP iniciando...");
        watch_bridge();
    }
    else
    {
        mcp_notify("Servidor MCP: falha ao iniciar " + python + " " + script + " (veja o log).");
    }
}

void LLBuildListener::bridgeReady(LLSD const& data)
{
    Response response(LLSD(), data);
    sBridgePid = data["pid"].asInteger();
    sBridgePort = data["port"].asInteger();
    sBridgeReady = true;
    // Capture chat and script dialogs from now on, not only after the first read_chat.
    ensureChatHooked();
    mcp_notify(stringize("Servidor MCP ligado em http://127.0.0.1:", sBridgePort, "/mcp"));
    watch_bridge();
}

namespace
{
    constexpr size_t CHAT_LOG_MAX = 500;
    constexpr size_t OBJECT_PROPS_MAX = 5000;

    // ObjectPropertiesFamily replies (name/desc/owner/permissions/sale) for any in-world object, by id.
    std::map<LLUUID, LLSD> sObjectProps;
    S32 sObjectPropsSeq = 0; // bumped on every reply, so clients can wait for a fresh one

    LLSD perm_mask_sd(U32 mask)
    {
        return llsd::map("copy", (mask & PERM_COPY) != 0,
                         "modify", (mask & PERM_MODIFY) != 0,
                         "transfer", (mask & PERM_TRANSFER) != 0,
                         "move", (mask & PERM_MOVE) != 0);
    }

    LLSD sale_sd(S32 sale_type, S32 price)
    {
        static const char* sale_names[] = { "not", "original", "copy", "contents" };
        return llsd::map("type", (sale_type >= 0 && sale_type < 4) ? sale_names[sale_type] : "not",
                         "price", price);
    }
    constexpr F32 TWIST_LINEAR_MAX = 180.f;
    constexpr F32 TWIST_CIRCULAR_MAX = 360.f;

    LLSD vec3_sd(const LLVector3& v)
    {
        return llsd::array(v.mV[VX], v.mV[VY], v.mV[VZ]);
    }

    LLVector3 sd_vec3(const LLSD& sd)
    {
        return LLVector3((F32)sd[0].asReal(), (F32)sd[1].asReal(), (F32)sd[2].asReal());
    }

    LLSD euler_deg_sd(const LLQuaternion& q)
    {
        F32 roll, pitch, yaw;
        q.getEulerAngles(&roll, &pitch, &yaw);
        return llsd::array(roll * RAD_TO_DEG, pitch * RAD_TO_DEG, yaw * RAD_TO_DEG);
    }

    LLQuaternion sd_euler_deg(const LLSD& sd)
    {
        LLQuaternion q;
        q.setQuat((F32)sd[0].asReal() * DEG_TO_RAD, (F32)sd[1].asReal() * DEG_TO_RAD, (F32)sd[2].asReal() * DEG_TO_RAD);
        return q;
    }

    // Looks up an object the agent may edit; fills the error on the response otherwise.
    LLViewerObject* find_editable(LLEventAPI::Response& response, const LLSD& id, bool need_modify = true)
    {
        LLViewerObject* obj = gObjectList.findObject(id.asUUID());
        if (!obj || obj->isDead())
        {
            response.error(stringize("Object ", id.asString(), " not found (is it in view?)"));
            return nullptr;
        }
        if (need_modify && !obj->permModify())
        {
            response.error(stringize("No modify permission on object ", id.asString()));
            return nullptr;
        }
        return obj;
    }

    void begin_agent_message(const char* name)
    {
        gMessageSystem->newMessage(name);
        gMessageSystem->nextBlockFast(_PREHASH_AgentData);
        gMessageSystem->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        gMessageSystem->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
    }

    // Builds volume params for the named basic prim shape. Returns false for an unknown name.
    bool params_for_shape(const std::string& shape, LLVolumeParams& params)
    {
        params.setBeginAndEndS(0.f, 1.f);
        params.setBeginAndEndT(0.f, 1.f);
        params.setRatio(1.f, 1.f);
        params.setShear(0.f, 0.f);
        if (shape == "box")              params.setType(LL_PCODE_PROFILE_SQUARE, LL_PCODE_PATH_LINE);
        else if (shape == "cylinder")    params.setType(LL_PCODE_PROFILE_CIRCLE, LL_PCODE_PATH_LINE);
        else if (shape == "prism")       params.setType(LL_PCODE_PROFILE_EQUALTRI, LL_PCODE_PATH_LINE);
        else if (shape == "sphere")      params.setType(LL_PCODE_PROFILE_CIRCLE_HALF, LL_PCODE_PATH_CIRCLE);
        else if (shape == "torus")     { params.setType(LL_PCODE_PROFILE_CIRCLE, LL_PCODE_PATH_CIRCLE); params.setRatio(1.f, 0.25f); }
        else if (shape == "tube")      { params.setType(LL_PCODE_PROFILE_SQUARE, LL_PCODE_PATH_CIRCLE); params.setRatio(1.f, 0.25f); }
        else if (shape == "ring")      { params.setType(LL_PCODE_PROFILE_EQUALTRI, LL_PCODE_PATH_CIRCLE); params.setRatio(1.f, 0.25f); }
        else return false;
        return true;
    }

    // Applies optional cut/hollow/twist/taper/shear/hole_size modifiers from data.
    void apply_shape_modifiers(const LLSD& data, LLVolumeParams& params)
    {
        bool circular = params.getPathParams().getCurveType() == LL_PCODE_PATH_CIRCLE;
        if (data.has("cut"))
        {
            F32 b = (F32)data["cut"][0].asReal(), e = (F32)data["cut"][1].asReal();
            if (circular) params.setBeginAndEndT(b, e);
            else          params.setBeginAndEndS(b, e);
        }
        if (data.has("hollow"))
        {
            params.setHollow(llclamp((F32)data["hollow"].asReal(), 0.f, 0.95f));
        }
        if (data.has("twist"))
        {
            F32 max = circular ? TWIST_CIRCULAR_MAX : TWIST_LINEAR_MAX;
            params.setTwistBegin((F32)data["twist"][0].asReal() / max);
            params.setTwistEnd((F32)data["twist"][1].asReal() / max);
        }
        if (data.has("taper") && !circular)
        {
            params.setRatio(1.f - (F32)data["taper"][0].asReal(), 1.f - (F32)data["taper"][1].asReal());
        }
        if (data.has("hole_size") && circular)
        {
            params.setRatio((F32)data["hole_size"][0].asReal(), (F32)data["hole_size"][1].asReal());
        }
        if (data.has("shear"))
        {
            params.setShear((F32)data["shear"][0].asReal(), (F32)data["shear"][1].asReal());
        }
    }

    LLSD object_info(LLViewerObject* obj, bool with_faces)
    {
        LLSD info = llsd::map(
            "id", obj->getID(),
            "local_id", LLSD::Integer(obj->getLocalID()),
            "region_pos", vec3_sd(obj->getPositionRegion()),
            "rot_deg", euler_deg_sd(obj->getRotationRegion()),
            "scale", vec3_sd(obj->getScale()),
            "is_root", obj->isRootEdit(),
            "root_id", obj->getRootEdit()->getID(),
            "is_mesh", obj->isMesh(),
            "can_modify", obj->permModify(),
            "you_own", obj->permYouOwner(),
            "physical", obj->flagUsePhysics(),
            "phantom", obj->flagPhantom(),
            "num_faces", LLSD::Integer(obj->getNumTEs()));
        if (obj->isRootEdit())
        {
            LLSD children = LLSD::emptyArray();
            for (LLViewerObject* child : obj->getChildren())
            {
                if (child && !child->isAvatar())
                {
                    children.append(child->getID());
                }
            }
            info["children"] = children;
        }
        if (obj->getPCode() == LL_PCODE_VOLUME && obj->getVolume() && !obj->isMesh()
            && !obj->getVolume()->getParams().isSculpt())
        {
            // Raw prim shape, so a client can restore it later with setVolumeParams (undo).
            info["volume_params"] = obj->getVolume()->getParams().asLLSD();
        }
        if (LLSelectNode* node = LLSelectMgr::getInstance()->getSelection()->findNode(obj);
            node && node->mValid)
        {
            info["name"] = node->mName;
            info["desc"] = node->mDescription;
            if (node->mPermissions)
            {
                const LLPermissions* perm = node->mPermissions;
                info["permissions"] = llsd::map(
                    "owner", perm_mask_sd(perm->getMaskOwner()),
                    "group", perm_mask_sd(perm->getMaskGroup()),
                    "everyone", perm_mask_sd(perm->getMaskEveryone()),
                    "next_owner", perm_mask_sd(perm->getMaskNextOwner()),
                    "owner_id", perm->getOwner(),
                    "creator_id", perm->getCreator());
                info["sale"] = sale_sd((S32)node->mSaleInfo.getSaleType(), node->mSaleInfo.getSalePrice());
            }
        }
        else if (auto found = sObjectProps.find(obj->getID()); found != sObjectProps.end())
        {
            info["name"] = found->second["name"];
            info["desc"] = found->second["desc"];
            info["owner_id"] = found->second["owner_id"];
            if (found->second.has("permissions"))
            {
                info["permissions"] = found->second["permissions"];
                info["sale"] = found->second["sale"];
                info["props_seq"] = found->second["seq"];
            }
        }
        if (with_faces)
        {
            LLSD faces = LLSD::emptyArray();
            for (U8 te = 0; te < obj->getNumTEs(); ++te)
            {
                const LLTextureEntry* entry = obj->getTE(te);
                if (!entry) continue;
                F32 s, t;
                entry->getScale(&s, &t);
                const LLColor4& c = entry->getColor();
                faces.append(llsd::map(
                    "face", LLSD::Integer(te),
                    "texture", entry->getID(),
                    "color", llsd::array(c.mV[VRED], c.mV[VGREEN], c.mV[VBLUE]),
                    "alpha", c.mV[VALPHA],
                    "repeats", llsd::array(s, t),
                    "offset", llsd::array(entry->mOffsetS, entry->mOffsetT),
                    "rotation_deg", entry->getRotation() * RAD_TO_DEG,
                    "fullbright", (bool)entry->getFullbright(),
                    "glow", entry->getGlow()));
            }
            info["faces"] = faces;
        }
        return info;
    }

    // Fetches an object's task inventory and replies once it arrives, then removes itself.
    class InventoryFetcher : public LLVOInventoryListener
    {
    public:
        InventoryFetcher(LLViewerObject* obj, const LLSD& request) : mRequest(request)
        {
            registerVOInventoryListener(obj, nullptr);
            obj->dirtyInventory();
            requestVOInventory();
        }

        void inventoryChanged(LLViewerObject* object, LLInventoryObject::object_list_t* inventory,
                              S32 serial_num, void* user_data) override
        {
            if (mDone) return;
            mDone = true;
            LLSD items = LLSD::emptyArray();
            if (inventory)
            {
                for (const auto& inv_obj : *inventory)
                {
                    LLInventoryItem* item = dynamic_cast<LLInventoryItem*>(inv_obj.get());
                    if (!item) continue;
                    items.append(llsd::map(
                        "id", item->getUUID(),
                        "name", item->getName(),
                        "desc", item->getDescription(),
                        "type", LLAssetType::lookup(item->getType()),
                        "asset_id", item->getAssetUUID()));
                }
            }
            sendReply(llsd::map("items", items, "serial", serial_num), mRequest);
            removeVOInventoryListener();
            // Called from inside the object's callback loop: defer our own deletion.
            doOnIdleOneTime([this]() { delete this; });
        }

    private:
        LLSD mRequest;
        bool mDone = false;
    };
}

LLBuildListener::LLBuildListener()
  : LLEventAPI("LLBuild",
               "API for building in-world: create and edit prims, faces, scripts, local mesh preview,\n"
               "and capture of nearby/script chat. Positions are region coordinates in meters,\n"
               "rotations are Euler angles in degrees [x, y, z].")
{
    add("getContext",
        "Return agent [\"region_pos\"], [\"rot_deg\"], [\"region\"] name and [\"selection_count\"]",
        &LLBuildListener::getContext,
        llsd::map("reply", LLSD()));

    add("getObject",
        "Return details of object [\"id\"]: transform, link info, permissions and per-face data",
        &LLBuildListener::getObject,
        llsd::map("id", LLSD(), "reply", LLSD()));

    add("getSelection",
        "Return the objects currently selected in the build tools, with names when known",
        &LLBuildListener::getSelection,
        llsd::map("reply", LLSD()));

    add("selectObjects",
        "Select the linksets of [\"ids\"] in the build tools (replacing the current selection)",
        &LLBuildListener::selectObjects,
        llsd::map("ids", LLSD(), "reply", LLSD()));

    add("createPrim",
        "Rez a prim of [\"shape\"] (box, cylinder, prism, sphere, torus, tube, ring) at [\"region_pos\"]\n"
        "with optional [\"scale\"] and [\"rot_deg\"] plus shape modifiers (see setShape).\n"
        "Replies with the new object's [\"id\"] once the simulator creates it.",
        &LLBuildListener::createPrim,
        llsd::map("shape", LLSD(), "region_pos", LLSD(), "reply", LLSD()));

    add("setTransform",
        "Set optional [\"region_pos\"], [\"rot_deg\"] and/or [\"scale\"] of object [\"id\"].\n"
        "Moving or rotating a root moves its whole linkset.",
        &LLBuildListener::setTransform,
        llsd::map("id", LLSD(), "reply", LLSD()));

    add("setShape",
        "Change the prim shape of [\"id\"]: optional [\"shape\"] plus [\"cut\"] [b,e] (0-1),\n"
        "[\"hollow\"] (0-0.95), [\"twist\"] [b,e] degrees, [\"taper\"] [x,y] (-1..1, straight shapes),\n"
        "[\"hole_size\"] [x,y] (round shapes), [\"shear\"] [x,y] (-0.5..0.5)",
        &LLBuildListener::setShape,
        llsd::map("id", LLSD(), "reply", LLSD()));

    add("setFaces",
        "Set appearance of [\"face\"] (-1 or absent = all faces) of [\"id\"]: optional [\"color\"] [r,g,b] (0-1),\n"
        "[\"alpha\"], [\"texture\"] UUID, [\"repeats\"] [s,t], [\"offset\"] [s,t], [\"rotation_deg\"],\n"
        "[\"fullbright\"], [\"glow\"]",
        &LLBuildListener::setFaces,
        llsd::map("id", LLSD(), "reply", LLSD()));

    add("setNameDesc",
        "Set optional [\"name\"] and/or [\"desc\"] of object [\"id\"]",
        &LLBuildListener::setNameDesc,
        llsd::map("id", LLSD(), "reply", LLSD()));

    add("setFlags",
        "Set optional [\"physical\"], [\"phantom\"], [\"temporary\"] booleans on object [\"id\"]",
        &LLBuildListener::setFlags,
        llsd::map("id", LLSD(), "reply", LLSD()));

    add("linkObjects",
        "Link [\"ids\"] into one linkset; the FIRST id becomes the root",
        &LLBuildListener::linkObjects,
        llsd::map("ids", LLSD(), "reply", LLSD()));

    add("unlinkObjects",
        "Unlink each of [\"ids\"] from its linkset",
        &LLBuildListener::unlinkObjects,
        llsd::map("ids", LLSD(), "reply", LLSD()));

    add("deleteObjects",
        "Take [\"ids\"] (root objects) into the Trash folder",
        &LLBuildListener::deleteObjects,
        llsd::map("ids", LLSD(), "reply", LLSD()));

    add("getObjectInventory",
        "Fetch the contents of object [\"id\"]; replies with [\"items\"] (id, name, type, asset_id)",
        &LLBuildListener::getObjectInventory,
        llsd::map("id", LLSD(), "reply", LLSD()));

    add("createScript",
        "Create a new, stopped script called [\"name\"] in object [\"id\"] (default script text).\n"
        "Use getObjectInventory to learn its item id, then uploadScript.",
        &LLBuildListener::createScript,
        llsd::map("id", LLSD(), "name", LLSD(), "reply", LLSD()));

    add("uploadScript",
        "Upload LSL [\"source\"] to script [\"item_id\"] in object [\"id\"] and compile it\n"
        "(optional [\"mono\"] default true, [\"running\"] default true).\n"
        "Replies with [\"compiled\"] and [\"errors\"].",
        &LLBuildListener::uploadScript,
        llsd::map("id", LLSD(), "item_id", LLSD(), "source", LLSD(), "reply", LLSD()));

    add("setScriptRunning",
        "Set script [\"item_id\"] in object [\"id\"] [\"running\"] true/false",
        &LLBuildListener::setScriptRunning,
        llsd::map("id", LLSD(), "item_id", LLSD(), "running", LLSD(), "reply", LLSD()));

    add("localMeshAdd",
        "Load a local mesh file [\"path\"] (.dae, .gltf, .glb) for client-side preview.\n"
        "Replies with the file [\"local_id\"]; poll localMeshList until its status is active.",
        &LLBuildListener::localMeshAdd,
        llsd::map("path", LLSD(), "reply", LLSD()));

    add("localMeshList",
        "List loaded local mesh files with [\"status\"], [\"objects\"] and recent [\"log\"]",
        &LLBuildListener::localMeshList,
        llsd::map("reply", LLSD()));

    add("localMeshApply",
        "Show local mesh [\"local_id\"] (sub-object [\"object_index\"], default 0) on object [\"id\"].\n"
        "A non-mesh prim is first converted to an (empty) mesh prim, which the simulator sees too.",
        &LLBuildListener::localMeshApply,
        llsd::map("id", LLSD(), "local_id", LLSD(), "reply", LLSD()));

    add("localMeshReload",
        "Reload local mesh [\"local_id\"] from disk; objects showing it update automatically",
        &LLBuildListener::localMeshReload,
        llsd::map("local_id", LLSD(), "reply", LLSD()));

    add("localMeshClear",
        "Stop showing a local mesh on object [\"id\"]",
        &LLBuildListener::localMeshClear,
        llsd::map("id", LLSD(), "reply", LLSD()));

    add("localMeshRemove",
        "Unload local mesh file [\"local_id\"]",
        &LLBuildListener::localMeshRemove,
        llsd::map("local_id", LLSD(), "reply", LLSD()));

    add("openMeshUpload",
        "Open the mesh upload window pre-loaded with model file [\"path\"]; the user reviews and pays",
        &LLBuildListener::openMeshUpload,
        llsd::map("path", LLSD(), "reply", LLSD()));

    add("rezFromInventory",
        "Rez agent inventory object [\"item_id\"] at [\"region_pos\"]. Replies with the new object's\n"
        "[\"id\"] when it is full-permission (else [\"id\"] is null: find it with nearby objects)",
        &LLBuildListener::rezFromInventory,
        llsd::map("item_id", LLSD(), "region_pos", LLSD(), "reply", LLSD()));

    add("takeObjects",
        "Take linksets [\"ids\"] into the Objects folder; [\"copy\"] true = Take Copy (leave them in world)",
        &LLBuildListener::takeObjects,
        llsd::map("ids", LLSD(), "reply", LLSD()));

    add("duplicateObject",
        "Duplicate linkset [\"id\"] shifted by [\"offset\"] [x,y,z]; replies with the copy's [\"id\"]",
        &LLBuildListener::duplicateObject,
        llsd::map("id", LLSD(), "offset", LLSD(), "reply", LLSD()));

    add("addInventoryToObject",
        "Copy agent inventory item [\"item_id\"] into object [\"id\"] (scripts start running).\n"
        "No-copy items are moved, so they are refused unless [\"allow_move\"] is true.",
        &LLBuildListener::addInventoryToObject,
        llsd::map("id", LLSD(), "item_id", LLSD(), "reply", LLSD()));

    add("removeObjectInventory",
        "Delete item [\"item_id\"] from object [\"id\"]'s contents",
        &LLBuildListener::removeObjectInventory,
        llsd::map("id", LLSD(), "item_id", LLSD(), "reply", LLSD()));

    add("createNotecard",
        "Create notecard [\"name\"] with [\"text\"] in the Notecards folder; with [\"id\"], also copy it\n"
        "into that object (and drop the inventory copy unless [\"keep_copy\"]). Replies when done.",
        &LLBuildListener::createNotecard,
        llsd::map("name", LLSD(), "text", LLSD(), "reply", LLSD()));

    add("uploadNotecard",
        "Replace the text of notecard [\"item_id\"] with [\"text\"]: inside object [\"id\"] if given,\n"
        "else in agent inventory",
        &LLBuildListener::uploadNotecard,
        llsd::map("item_id", LLSD(), "text", LLSD(), "reply", LLSD()));

    add("readItemText",
        "Return the [\"text\"] of a script or notecard [\"item_id\"], inside object [\"id\"] if given,\n"
        "else in agent inventory (needs permission to see it)",
        &LLBuildListener::readItemText,
        llsd::map("item_id", LLSD(), "reply", LLSD()));

    add("resetScript",
        "Reset script [\"item_id\"] in object [\"id\"]",
        &LLBuildListener::resetScript,
        llsd::map("id", LLSD(), "item_id", LLSD(), "reply", LLSD()));

    add("getObjectCosts",
        "Return land impact figures of [\"id\"] and its linkset (first call may return 0 while the\n"
        "simulator is asked; call again a moment later)",
        &LLBuildListener::getObjectCosts,
        llsd::map("id", LLSD(), "reply", LLSD()));

    add("getParcelInfo",
        "Return the agent's parcel name/area and prim usage and limits (parcel and region-wide)",
        &LLBuildListener::getParcelInfo,
        llsd::map("reply", LLSD()));

    add("canBuildAt",
        "Whether the agent may create objects at [\"region_pos\"] (agent's region): replies\n"
        "[\"allowed\"] true/false, or undefined when it cannot be known from here (another\n"
        "parcel that is not the agent's own land), plus [\"reason\"] and [\"in_agent_parcel\"]",
        &LLBuildListener::canBuildAt,
        llsd::map("region_pos", LLSD(), "reply", LLSD()));

    add("setRenderMaterial",
        "Apply PBR material asset [\"material_id\"] (null clears it) to [\"face\"] (-1 = all) of [\"id\"]",
        &LLBuildListener::setRenderMaterial,
        llsd::map("id", LLSD(), "reply", LLSD()));

    add("setExtraParams",
        "Set on object [\"id\"] any of: [\"light\"] (false or {color, intensity, radius, falloff}),\n"
        "[\"flexible\"] (false or {softness, gravity, drag, wind, tension, force}),\n"
        "[\"physics_shape\"] (prim|none|convex), [\"click_action\"] (touch|sit|buy|pay|open|play|\n"
        "open_media|zoom|disabled), [\"sculpt\"] {texture, type: sphere|torus|plane|cylinder}",
        &LLBuildListener::setExtraParams,
        llsd::map("id", LLSD(), "reply", LLSD()));

    add("uploadFile",
        "Upload [\"path\"] (.png/.tga/.jpg/.bmp texture, .wav sound, .anim animation) as [\"name\"].\n"
        "Refused when the upload cost exceeds [\"max_cost\"] (default 0). A .bvh opens the animation\n"
        "upload window instead, for the user to review. Replies with [\"expected_cost\"].",
        &LLBuildListener::uploadFile,
        llsd::map("path", LLSD(), "reply", LLSD()));

    add("requestObjectProperties",
        "Ask the simulator for name, description and owner of objects [\"ids\"];\n"
        "read them a moment later with getObjectProperties",
        &LLBuildListener::requestObjectProperties,
        llsd::map("ids", LLSD(), "reply", LLSD()));

    add("getObjectProperties",
        "Return cached [\"objects\"] {id: {name, desc, owner_id, owner_name, group_id,\n"
        "creator_id}} for [\"ids\"]; ids not received yet are listed in [\"missing\"]",
        &LLBuildListener::getObjectProperties,
        llsd::map("ids", LLSD(), "reply", LLSD()));

    add("getWornItems",
        "Return what the avatar wears: [\"wearables\"] (body parts and clothing layers)\n"
        "and [\"attachments\"] (attached objects with attachment point)",
        &LLBuildListener::getWornItems,
        llsd::map("reply", LLSD()));

    add("getAvatarPose",
        "Return the agent's animated skeleton in region coordinates ([\"joints\"]: pelvis, torso,\n"
        "chest, head, hips, knees, ankles, feet), plus [\"sitting\"] and [\"sit_root_id\"]",
        &LLBuildListener::getAvatarPose,
        llsd::map("reply", LLSD()));

    add("snapshot",
        "Save a PNG of the 3D view (no UI) to [\"path\"] ([\"width\"]/[\"height\"], default 1024x768).\n"
        "Optional [\"camera_pos\"] and [\"focus\"] (region coordinates) place the camera first;\n"
        "the picture is taken after [\"settle\"] seconds (default 1). [\"show_ui\"] true also draws\n"
        "the UI and hover text, [\"show_hud\"] the HUD attachments. Replies with [\"path\"].",
        &LLBuildListener::snapshot,
        llsd::map("path", LLSD(), "reply", LLSD()));

    add("resetCamera",
        "Put the camera back behind the avatar",
        &LLBuildListener::resetCamera,
        llsd::map("reply", LLSD()));

    add("getChat",
        "Return captured nearby, owner-say and script-debug chat with sequence number > [\"since\"]\n"
        "(default 0) as [\"messages\"], plus [\"last_seq\"]",
        &LLBuildListener::getChat,
        llsd::map("reply", LLSD()));

    // <FS:MCP> bridge status, permissions/sale, wearing, IM, teleport, export, undo support
    add("bridgeReady",
        "Called by the MCP bridge once its HTTP server listens: [\"pid\"], [\"port\"]",
        &LLBuildListener::bridgeReady,
        llsd::map("pid", LLSD(), "port", LLSD(), "reply", LLSD()));

    add("setPermissions",
        "Set permissions of the linkset of [\"id\"]: optional booleans [\"next_owner_copy\"],\n"
        "[\"next_owner_modify\"], [\"next_owner_transfer\"], [\"everyone_copy\"], [\"everyone_move\"],\n"
        "[\"group_share\"] (group may modify)",
        &LLBuildListener::setPermissions,
        llsd::map("id", LLSD(), "reply", LLSD()));

    add("setSaleInfo",
        "Put the linkset of [\"id\"] for sale: [\"type\"] not/original/copy/contents, [\"price\"] L$",
        &LLBuildListener::setSaleInfo,
        llsd::map("id", LLSD(), "type", LLSD(), "reply", LLSD()));

    add("wearItem",
        "Wear/attach inventory item [\"item_id\"]; optional [\"replace\"] and [\"attach_point\"] name",
        &LLBuildListener::wearItem,
        llsd::map("item_id", LLSD(), "reply", LLSD()));

    add("removeItem",
        "Take off / detach the worn inventory item [\"item_id\"]",
        &LLBuildListener::removeItem,
        llsd::map("item_id", LLSD(), "reply", LLSD()));

    add("getIM",
        "Open IM and group chat sessions with their latest [\"max_messages\"] messages (newest first)",
        &LLBuildListener::getIM,
        llsd::map("reply", LLSD()));

    add("confirmTeleport",
        "Ask the user (Yes/Cancel dialog) to teleport to [\"region\"] at [\"x\"], [\"y\"], [\"z\"];\n"
        "replies [\"accepted\"] once they answer",
        &LLBuildListener::confirmTeleport,
        llsd::map("region", LLSD(), "x", LLSD(), "y", LLSD(), "z", LLSD(), "reply", LLSD()));

    add("openExport",
        "Select the linkset of [\"id\"] and open Firestorm's object export window",
        &LLBuildListener::openExport,
        llsd::map("id", LLSD(), "reply", LLSD()));

    add("setVolumeParams",
        "Restore the raw prim shape [\"params\"] (as returned in getObject's volume_params) of [\"id\"]",
        &LLBuildListener::setVolumeParams,
        llsd::map("id", LLSD(), "params", LLSD(), "reply", LLSD()));

    // <FS:MCP> HUDs, touching, script dialogs
    add("getHudObjects",
        "List the worn HUD attachments with every prim of each (id, local position, scale, faces)",
        &LLBuildListener::getHudObjects,
        llsd::map("reply", LLSD()));

    add("getScreenInfo",
        "Window size in raw pixels and the UI scale, for mapping screenshots to clicks",
        &LLBuildListener::getScreenInfo,
        llsd::map("reply", LLSD()));

    add("touchObject",
        "Touch (click) prim [\"id\"] (in-world or HUD) like a left click: optional [\"face\"] and\n"
        "[\"st\"] [s,t] 0-1 texture point, [\"hold\"] seconds before releasing (default 0.1)",
        &LLBuildListener::touchObject,
        llsd::map("id", LLSD(), "reply", LLSD()));

    add("touchAt",
        "Left-click whatever is under raw window pixel [\"x\"], [\"y\"] (top-left origin): HUDs first,\n"
        "then in-world objects. Replies with what was touched. Optional [\"hold\"] seconds.",
        &LLBuildListener::touchAt,
        llsd::map("x", LLSD(), "y", LLSD(), "reply", LLSD()));

    add("getScriptDialogs",
        "Open script dialogs (llDialog blue menus and llTextBox): id, object, message, buttons",
        &LLBuildListener::getScriptDialogs,
        llsd::map("reply", LLSD()));

    add("answerScriptDialog",
        "Answer script dialog [\"id\"]: press [\"button\"] (label), or for a text box send [\"text\"];\n"
        "[\"ignore\"] true just closes it",
        &LLBuildListener::answerScriptDialog,
        llsd::map("id", LLSD(), "reply", LLSD()));
}

namespace { void ensure_script_dialogs_hooked(); } // defined with the HUD/dialog ops below
void handle_object_sit(LLViewerObject* object, const LLVector3& offset); // llviewermenu.cpp

void LLBuildListener::ensureChatHooked()
{
    ensure_script_dialogs_hooked(); // <FS:MCP> so no llDialog shown meanwhile is missed
    if (mChatConnection.connected() || !LLNotificationsUI::LLNotificationManager::instanceExists())
    {
        return;
    }
    auto handler = LLNotificationsUI::LLNotificationManager::instance().getChatHandler();
    if (handler)
    {
        mChatConnection = handler->addNewChatCallback([this](const LLSD& chat) { onChat(chat); });
    }
}

void LLBuildListener::onChat(const LLSD& chat)
{
    LLSD entry(chat);
    entry["seq"] = ++mChatSeq;
    mChatLog.push_back(entry);
    while (mChatLog.size() > CHAT_LOG_MAX)
    {
        mChatLog.pop_front();
    }
}

void LLBuildListener::getChat(LLSD const& data)
{
    Response response(LLSD(), data);
    ensureChatHooked();
    S32 since = data["since"].asInteger();
    LLSD messages = LLSD::emptyArray();
    for (const LLSD& entry : mChatLog)
    {
        if (entry["seq"].asInteger() > since)
        {
            messages.append(entry);
        }
    }
    response["messages"] = messages;
    response["last_seq"] = mChatSeq;
    response["capturing"] = mChatConnection.connected();
}

void LLBuildListener::getContext(LLSD const& data)
{
    Response response(LLSD(), data);
    ensureChatHooked();
    LLViewerRegion* region = gAgent.getRegion();
    if (!region)
    {
        return response.error("Not logged in to a region");
    }
    response["region"] = region->getName();
    response["region_pos"] = vec3_sd(gAgent.getPositionAgent() - region->getOriginAgent());
    response["rot_deg"] = euler_deg_sd(gAgent.getQuat());
    response["selection_count"] = LLSelectMgr::getInstance()->getSelection()->getObjectCount();
    const LLVector3d& origin = region->getOriginGlobal();
    response["region_origin_global"] = llsd::array(origin.mdV[VX], origin.mdV[VY], origin.mdV[VZ]);
    if (isAgentAvatarValid())
    {
        response["sitting"] = gAgentAvatarp->isSitting();
        if (LLViewerObject* seat = dynamic_cast<LLViewerObject*>(gAgentAvatarp->getParent()))
        {
            response["sit_root_id"] = seat->getRootEdit()->getID();
        }
    }
    response["max_prim_scale"] = LLWorld::getInstance()->getRegionMaxPrimScale();
}

void LLBuildListener::getObject(LLSD const& data)
{
    Response response(LLSD(), data);
    if (LLViewerObject* obj = find_editable(response, data["id"], false))
    {
        response["object"] = object_info(obj, true);
    }
}

void LLBuildListener::getSelection(LLSD const& data)
{
    Response response(LLSD(), data);
    LLSD objects = LLSD::emptyArray();
    LLObjectSelectionHandle selection = LLSelectMgr::getInstance()->getSelection();
    for (LLObjectSelection::iterator it = selection->begin(); it != selection->end(); ++it)
    {
        LLViewerObject* obj = (*it)->getObject();
        if (obj && !obj->isDead())
        {
            objects.append(object_info(obj, false));
        }
    }
    response["objects"] = objects;
}

void LLBuildListener::selectObjects(LLSD const& data)
{
    Response response(LLSD(), data);
    std::vector<LLViewerObject*> objects;
    for (const LLSD& id : llsd::inArray(data["ids"]))
    {
        LLViewerObject* obj = find_editable(response, id, false);
        if (!obj) return;
        objects.push_back(obj);
    }
    LLSelectMgr::getInstance()->deselectAll();
    for (LLViewerObject* obj : objects)
    {
        LLSelectMgr::getInstance()->selectObjectAndFamily(obj, true);
    }
}

void LLBuildListener::createPrim(LLSD const& data)
{
    LLViewerRegion* region = gAgent.getRegion();
    LLVolumeParams params;
    if (!region)
    {
        Response(LLSD(), data).error("Not logged in to a region");
        return;
    }
    if (!params_for_shape(data["shape"].asString(), params))
    {
        Response(LLSD(), data).error(stringize("Unknown shape ", std::quoted(data["shape"].asString())));
        return;
    }
    apply_shape_modifiers(data, params);

    F32 max_scale = LLWorld::getInstance()->getRegionMaxPrimScale();
    F32 min_scale = LLWorld::getInstance()->getRegionMinPrimScale();
    LLVector3 scale = data.has("scale") ? sd_vec3(data["scale"]) : LLVector3(0.5f, 0.5f, 0.5f);
    for (S32 i = 0; i < 3; ++i)
    {
        scale.mV[i] = llclamp(scale.mV[i], min_scale, max_scale);
    }
    LLQuaternion rotation = data.has("rot_deg") ? sd_euler_deg(data["rot_deg"]) : LLQuaternion();
    LLVector3 pos = sd_vec3(data["region_pos"]);

    // The simulator assigns the UUID; catch the object when it arrives created-selected.
    LLSD request(data);
    gObjectList.setNewObjectCallback([request](LLViewerObject* obj) -> bool
    {
        sendReply(llsd::map("id", obj->getID(), "object", object_info(obj, false)), request);
        return false; // keep the normal behaviour (select it in the build tools)
    });

    LLMessageSystem* msg = gMessageSystem;
    msg->newMessageFast(_PREHASH_ObjectAdd);
    msg->nextBlockFast(_PREHASH_AgentData);
    msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
    msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
    msg->addUUIDFast(_PREHASH_GroupID, FSCommon::getGroupForRezzing());
    msg->nextBlockFast(_PREHASH_ObjectData);
    msg->addU8Fast(_PREHASH_Material, LL_MCODE_WOOD);
    msg->addU32Fast(_PREHASH_AddFlags, FLAGS_CREATE_SELECTED);
    LLVolumeMessage::packVolumeParams(&params, msg);
    msg->addU8Fast(_PREHASH_PCode, LL_PCODE_VOLUME);
    msg->addVector3Fast(_PREHASH_Scale, scale);
    msg->addQuatFast(_PREHASH_Rotation, rotation);
    msg->addVector3Fast(_PREHASH_RayStart, pos);
    msg->addVector3Fast(_PREHASH_RayEnd, pos);
    msg->addU8Fast(_PREHASH_BypassRaycast, (U8)1);
    msg->addU8Fast(_PREHASH_RayEndIsIntersection, (U8)0);
    msg->addU8Fast(_PREHASH_State, 0);
    msg->addUUIDFast(_PREHASH_RayTargetID, LLUUID::null);
    msg->sendReliable(region->getHost());
    // Reply is sent from the new-object callback.
}

void LLBuildListener::setTransform(LLSD const& data)
{
    Response response(LLSD(), data);
    LLViewerObject* obj = find_editable(response, data["id"]);
    if (!obj) return;
    LLViewerRegion* region = obj->getRegion();
    if (!region) return response.error("Object has no region");

    LLViewerObject* root = obj->getRootEdit();
    bool is_root = (root == obj);

    // Values sent are parent-relative for child prims, region-relative for roots.
    LLVector3 local_pos = obj->getPosition();
    LLQuaternion local_rot = obj->getRotation();
    U8 type = 0;
    if (data.has("region_pos"))
    {
        LLVector3 pos = sd_vec3(data["region_pos"]);
        local_pos = is_root ? pos : (pos - root->getPositionRegion()) * ~root->getRotationRegion();
        type |= UPD_POSITION;
    }
    if (data.has("rot_deg"))
    {
        LLQuaternion rot = sd_euler_deg(data["rot_deg"]);
        local_rot = is_root ? rot : rot * ~root->getRotationRegion();
        type |= UPD_ROTATION;
    }

    auto send = [&](U8 upd, const LLVector3* scale)
    {
        U8 buf[64];
        S32 offset = 0;
        if (upd & UPD_POSITION)
        {
            htolememcpy(&buf[offset], local_pos.mV, MVT_LLVector3, 12);
            offset += 12;
        }
        if (upd & UPD_ROTATION)
        {
            LLVector3 packed = local_rot.packToVector3();
            htolememcpy(&buf[offset], packed.mV, MVT_LLQuaternion, 12);
            offset += 12;
        }
        if (upd & UPD_SCALE)
        {
            htolememcpy(&buf[offset], scale->mV, MVT_LLVector3, 12);
            offset += 12;
        }
        begin_agent_message("MultipleObjectUpdate");
        gMessageSystem->nextBlockFast(_PREHASH_ObjectData);
        gMessageSystem->addU32Fast(_PREHASH_ObjectLocalID, obj->getLocalID());
        gMessageSystem->addU8Fast(_PREHASH_Type, upd);
        gMessageSystem->addBinaryDataFast(_PREHASH_Data, buf, offset);
        gMessageSystem->sendReliable(region->getHost());
    };

    if (type)
    {
        // For a root, move/rotate the whole linkset.
        send(is_root ? (type | UPD_LINKED_SETS) : type, nullptr);
    }
    if (data.has("scale"))
    {
        LLVector3 scale = sd_vec3(data["scale"]);
        F32 max_scale = LLWorld::getInstance()->getRegionMaxPrimScale();
        F32 min_scale = LLWorld::getInstance()->getRegionMinPrimScale();
        for (S32 i = 0; i < 3; ++i)
        {
            scale.mV[i] = llclamp(scale.mV[i], min_scale, max_scale);
        }
        // Scale only this prim, leaving the rest of the linkset alone. Always send the
        // (new or current) position with it, as LLPanelObject::sendScale does: a scale-only
        // update on a child prim makes the simulator misplace it.
        send(UPD_SCALE | UPD_POSITION, &scale);
    }
    if (!type && !data.has("scale"))
    {
        response.warn("Nothing to change: pass region_pos, rot_deg and/or scale");
    }
}

void LLBuildListener::setShape(LLSD const& data)
{
    Response response(LLSD(), data);
    LLViewerObject* obj = find_editable(response, data["id"]);
    if (!obj) return;
    if (obj->getPCode() != LL_PCODE_VOLUME || !obj->getVolume())
    {
        return response.error("Object is not a prim");
    }
    LLVolumeParams params = obj->getVolume()->getParams();
    if (data.has("shape"))
    {
        if (!params_for_shape(data["shape"].asString(), params))
        {
            return response.error(stringize("Unknown shape ", std::quoted(data["shape"].asString())));
        }
    }
    else if (obj->isMesh() || params.isSculpt())
    {
        return response.error("Object is a mesh/sculpt; pass [\"shape\"] to turn it back into a prim");
    }
    apply_shape_modifiers(data, params);
    obj->updateVolume(params);
}

void LLBuildListener::setFaces(LLSD const& data)
{
    Response response(LLSD(), data);
    LLViewerObject* obj = find_editable(response, data["id"]);
    if (!obj) return;

    S32 face = data.has("face") ? data["face"].asInteger() : -1;
    U8 num_tes = obj->getNumTEs();
    if (face >= (S32)num_tes)
    {
        return response.error(stringize("Object has only ", (S32)num_tes, " faces"));
    }
    U8 first = face < 0 ? 0 : (U8)face;
    U8 last = face < 0 ? num_tes : (U8)(face + 1);
    for (U8 te = first; te < last; ++te)
    {
        const LLTextureEntry* entry = obj->getTE(te);
        if (!entry) continue;
        if (data.has("color") || data.has("alpha"))
        {
            LLColor4 color = entry->getColor();
            if (data.has("color"))
            {
                color.mV[VRED] = (F32)data["color"][0].asReal();
                color.mV[VGREEN] = (F32)data["color"][1].asReal();
                color.mV[VBLUE] = (F32)data["color"][2].asReal();
            }
            if (data.has("alpha"))
            {
                color.mV[VALPHA] = llclamp((F32)data["alpha"].asReal(), 0.f, 1.f);
            }
            obj->setTEColor(te, color);
        }
        if (data.has("texture"))
        {
            obj->setTETexture(te, data["texture"].asUUID());
        }
        if (data.has("repeats"))
        {
            obj->setTEScale(te, (F32)data["repeats"][0].asReal(), (F32)data["repeats"][1].asReal());
        }
        if (data.has("offset"))
        {
            obj->setTEOffset(te, (F32)data["offset"][0].asReal(), (F32)data["offset"][1].asReal());
        }
        if (data.has("rotation_deg"))
        {
            obj->setTERotation(te, (F32)data["rotation_deg"].asReal() * DEG_TO_RAD);
        }
        if (data.has("fullbright"))
        {
            obj->setTEFullbright(te, data["fullbright"].asBoolean() ? 1 : 0);
        }
        if (data.has("glow"))
        {
            obj->setTEGlow(te, llclamp((F32)data["glow"].asReal(), 0.f, 1.f));
        }
    }
    obj->sendTEUpdate();
}

void LLBuildListener::setNameDesc(LLSD const& data)
{
    Response response(LLSD(), data);
    LLViewerObject* obj = find_editable(response, data["id"]);
    if (!obj || !obj->getRegion()) return;
    if (data.has("name"))
    {
        begin_agent_message("ObjectName");
        gMessageSystem->nextBlockFast(_PREHASH_ObjectData);
        gMessageSystem->addU32Fast(_PREHASH_LocalID, obj->getLocalID());
        gMessageSystem->addStringFast(_PREHASH_Name, data["name"].asString());
        gMessageSystem->sendReliable(obj->getRegion()->getHost());
    }
    if (data.has("desc"))
    {
        begin_agent_message("ObjectDescription");
        gMessageSystem->nextBlockFast(_PREHASH_ObjectData);
        gMessageSystem->addU32Fast(_PREHASH_LocalID, obj->getLocalID());
        gMessageSystem->addStringFast(_PREHASH_Description, data["desc"].asString());
        gMessageSystem->sendReliable(obj->getRegion()->getHost());
    }
}

void LLBuildListener::setFlags(LLSD const& data)
{
    Response response(LLSD(), data);
    LLViewerObject* obj = find_editable(response, data["id"]);
    if (!obj) return;
    // Flags live on the root; set them without sending, then send one update.
    LLViewerObject* root = obj->getRootEdit();
    if (data.has("physical"))  root->setFlagsWithoutUpdate(FLAGS_USE_PHYSICS, data["physical"].asBoolean());
    if (data.has("phantom"))   root->setFlagsWithoutUpdate(FLAGS_PHANTOM, data["phantom"].asBoolean());
    if (data.has("temporary")) root->setFlagsWithoutUpdate(FLAGS_TEMPORARY_ON_REZ, data["temporary"].asBoolean());
    root->updateFlags();
}

void LLBuildListener::linkObjects(LLSD const& data)
{
    Response response(LLSD(), data);
    std::vector<LLViewerObject*> objects;
    for (const LLSD& id : llsd::inArray(data["ids"]))
    {
        LLViewerObject* obj = find_editable(response, id);
        if (!obj) return;
        objects.push_back(obj->getRootEdit());
    }
    if (objects.size() < 2)
    {
        return response.error("Need at least two objects to link");
    }
    LLViewerRegion* region = objects.front()->getRegion();
    begin_agent_message("ObjectLink");
    for (LLViewerObject* obj : objects)
    {
        if (obj->getRegion() != region)
        {
            return response.error("All objects must be in the same region");
        }
        gMessageSystem->nextBlockFast(_PREHASH_ObjectData);
        gMessageSystem->addU32Fast(_PREHASH_ObjectLocalID, obj->getLocalID());
    }
    gMessageSystem->sendReliable(region->getHost());
}

void LLBuildListener::unlinkObjects(LLSD const& data)
{
    Response response(LLSD(), data);
    for (const LLSD& id : llsd::inArray(data["ids"]))
    {
        LLViewerObject* obj = find_editable(response, id);
        if (!obj || !obj->getRegion()) return;
        begin_agent_message("ObjectDelink");
        gMessageSystem->nextBlockFast(_PREHASH_ObjectData);
        gMessageSystem->addU32Fast(_PREHASH_ObjectLocalID, obj->getLocalID());
        gMessageSystem->sendReliable(obj->getRegion()->getHost());
    }
}

void LLBuildListener::deleteObjects(LLSD const& data)
{
    Response response(LLSD(), data);
    LLUUID trash_id = gInventory.findCategoryUUIDForType(LLFolderType::FT_TRASH);
    for (const LLSD& id : llsd::inArray(data["ids"]))
    {
        LLViewerObject* obj = find_editable(response, id);
        if (!obj || !obj->getRegion()) return;
        if (!obj->permYouOwner())
        {
            return response.error(stringize("You do not own object ", id.asString()));
        }
        obj = obj->getRootEdit();
        LLMessageSystem* msg = gMessageSystem;
        msg->newMessageFast(_PREHASH_DeRezObject);
        msg->nextBlockFast(_PREHASH_AgentData);
        msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        msg->nextBlockFast(_PREHASH_AgentBlock);
        msg->addUUIDFast(_PREHASH_GroupID, gAgent.getGroupID());
        msg->addU8Fast(_PREHASH_Destination, (U8)DRD_TRASH);
        msg->addUUIDFast(_PREHASH_DestinationID, trash_id);
        LLUUID tid;
        tid.generate();
        msg->addUUIDFast(_PREHASH_TransactionID, tid);
        msg->addU8Fast(_PREHASH_PacketCount, 1);
        msg->addU8Fast(_PREHASH_PacketNumber, 1);
        msg->nextBlockFast(_PREHASH_ObjectData);
        msg->addU32Fast(_PREHASH_ObjectLocalID, obj->getLocalID());
        msg->sendReliable(obj->getRegion()->getHost());
    }
}

void LLBuildListener::getObjectInventory(LLSD const& data)
{
    LLViewerObject* obj = gObjectList.findObject(data["id"].asUUID());
    if (!obj || obj->isDead())
    {
        Response(LLSD(), data).error(stringize("Object ", data["id"].asString(), " not found"));
        return;
    }
    new InventoryFetcher(obj, data); // replies and deletes itself when the inventory arrives
}

void LLBuildListener::createScript(LLSD const& data)
{
    Response response(LLSD(), data);
    LLViewerObject* obj = find_editable(response, data["id"]);
    if (!obj) return;
    std::string name = data["name"].asString();
    if (name.empty())
    {
        return response.error("Script name is required");
    }

    LLPermissions perm;
    perm.init(gAgent.getID(), gAgent.getID(), LLUUID::null, LLUUID::null);
    perm.initMasks(PERM_ALL,
                   PERM_ALL,
                   LLFloaterPerms::getEveryonePerms("Scripts"),
                   LLFloaterPerms::getGroupPerms("Scripts"),
                   PERM_MOVE | LLFloaterPerms::getNextOwnerPerms("Scripts"));
    std::string desc;
    LLViewerAssetType::generateDescriptionFor(LLAssetType::AT_LSL_TEXT, desc);
    LLPointer<LLViewerInventoryItem> new_item =
        new LLViewerInventoryItem(LLUUID::null, LLUUID::null, perm, LLUUID::null,
                                  LLAssetType::AT_LSL_TEXT, LLInventoryType::IT_LSL,
                                  name, desc, LLSaleInfo::DEFAULT,
                                  LLInventoryItemFlags::II_FLAGS_NONE, time_corrected());
    // Created stopped so the default "Hello, Avatar!" never runs; uploadScript starts it.
    obj->saveScript(new_item, false, true);
}

void LLBuildListener::uploadScript(LLSD const& data)
{
    LLViewerObject* obj = gObjectList.findObject(data["id"].asUUID());
    if (!obj || obj->isDead() || !obj->getRegion())
    {
        Response(LLSD(), data).error(stringize("Object ", data["id"].asString(), " not found"));
        return;
    }
    if (!obj->permModify())
    {
        Response(LLSD(), data).error("No modify permission on object");
        return;
    }
    std::string url = obj->getRegion()->getCapability("UpdateScriptTask");
    if (url.empty())
    {
        Response(LLSD(), data).error("Region does not offer the UpdateScriptTask capability");
        return;
    }

    bool mono = data.has("mono") ? data["mono"].asBoolean() : true;
    bool running = data.has("running") ? data["running"].asBoolean() : true;
    LLSD request(data);
    LLResourceUploadInfo::ptr_t info(std::make_shared<LLScriptAssetUpload>(
        obj->getID(), data["item_id"].asUUID(),
        mono ? LLScriptAssetUpload::MONO : LLScriptAssetUpload::LSL2,
        running, LLUUID::null, data["source"].asString(),
        [request](LLUUID item_id, LLUUID task_id, LLUUID new_asset_id, LLSD result)
        {
            LLSD errors = LLSD::emptyArray();
            for (const LLSD& line : llsd::inArray(result["errors"]))
            {
                std::string text = line.asString();
                LLStringUtil::stripNonprintable(text);
                errors.append(text);
            }
            sendReply(llsd::map("compiled", result["compiled"].asBoolean(),
                                "errors", errors,
                                "asset_id", new_asset_id), request);
        },
        [request](LLUUID item_id, LLUUID task_id, LLSD result, std::string reason) -> bool
        {
            sendReply(llsd::map("compiled", false,
                                "error", stringize("Upload failed: ", reason)), request);
            return false;
        }));
    LLViewerAssetUpload::EnqueueInventoryUpload(url, info);
}

void LLBuildListener::setScriptRunning(LLSD const& data)
{
    Response response(LLSD(), data);
    LLViewerObject* obj = find_editable(response, data["id"]);
    if (!obj || !obj->getRegion()) return;
    LLMessageSystem* msg = gMessageSystem;
    msg->newMessageFast(_PREHASH_SetScriptRunning);
    msg->nextBlockFast(_PREHASH_AgentData);
    msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
    msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
    msg->nextBlockFast(_PREHASH_Script);
    msg->addUUIDFast(_PREHASH_ObjectID, obj->getID());
    msg->addUUIDFast(_PREHASH_ItemID, data["item_id"].asUUID());
    msg->addBOOLFast(_PREHASH_Running, data["running"].asBoolean());
    msg->sendReliable(obj->getRegion()->getHost());
}

void LLBuildListener::localMeshAdd(LLSD const& data)
{
    Response response(LLSD(), data);
    std::string path = data["path"].asString();
    LLLocalMeshSystem* system = LLLocalMeshSystem::getInstance();
    system->addFile(path, true);
    // addFile pushes the new entry at the end of the list.
    auto files = system->getFileInfoVector();
    if (files.empty() || files.back().mLocalID.isNull())
    {
        // Failed entries have a null id; drop them so they don't pile up.
        system->deleteFile(LLUUID::null);
        return response.error(stringize("Could not load ", std::quoted(path), " (missing file or unsupported extension)"));
    }
    response["local_id"] = files.back().mLocalID;
    response["name"] = files.back().mName;
}

void LLBuildListener::localMeshList(LLSD const& data)
{
    Response response(LLSD(), data);
    static const char* status_names[] = { "none", "loading", "active", "error" };
    LLLocalMeshSystem* system = LLLocalMeshSystem::getInstance();
    LLSD files = LLSD::emptyArray();
    for (const auto& file : system->getFileInfoVector())
    {
        LLSD objects = LLSD::emptyArray();
        for (const auto& name : file.mObjectList)
        {
            objects.append(name);
        }
        LLSD lods = LLSD::emptyArray();
        for (bool lod : file.mLODAvailability)
        {
            lods.append(lod);
        }
        LLSD log = LLSD::emptyArray();
        auto lines = system->getFileLog(file.mLocalID);
        size_t start = lines.size() > 20 ? lines.size() - 20 : 0;
        for (size_t i = start; i < lines.size(); ++i)
        {
            log.append(lines[i]);
        }
        files.append(llsd::map(
            "local_id", file.mLocalID,
            "name", file.mName,
            "status", status_names[llclamp((S32)file.mStatus, 0, 3)],
            "objects", objects,
            "lods", lods,
            "log", log));
    }
    response["files"] = files;
}

void LLBuildListener::localMeshApply(LLSD const& data)
{
    Response response(LLSD(), data);
    LLViewerObject* obj = find_editable(response, data["id"], false);
    if (!obj) return;
    if (obj->getPCode() != LL_PCODE_VOLUME || !obj->getVolume())
    {
        return response.error("Target must be a prim or mesh object");
    }
    if (!obj->isMesh())
    {
        if (!obj->permModify())
        {
            return response.error("Converting a prim to mesh needs modify permission");
        }
        // Same conversion the Local Mesh floater does for freshly rezzed prims.
        LLVolumeParams params = obj->getVolume()->getParams();
        obj->setParameterEntryInUse(LLNetworkData::PARAMS_SCULPT, true, true);
        LLSculptParams* sculpt = obj->getSculptParams();
        if (!sculpt)
        {
            return response.error("Could not convert prim to mesh");
        }
        sculpt->setSculptTexture(LLUUID::null, LL_SCULPT_TYPE_MESH);
        params.setSculptID(LLUUID::null, LL_SCULPT_TYPE_MESH);
        obj->updateVolume(params);
        response["converted"] = true;
    }

    LLUUID local_id = data["local_id"].asUUID();
    S32 index = data.has("object_index") ? data["object_index"].asInteger() : 0;
    bool use_scale = data["use_scale"].asBoolean();
    bool found = false;
    for (const auto& file : LLLocalMeshSystem::getInstance()->getFileInfoVector())
    {
        if (file.mLocalID != local_id) continue;
        found = true;
        if (file.mStatus != LLLocalMeshFile::STATUS_ACTIVE)
        {
            return response.error("Local mesh is not active yet (still loading or failed); check localMeshList");
        }
        if (index < 0 || index >= (S32)file.mObjectList.size())
        {
            return response.error(stringize("object_index out of range; file has ", file.mObjectList.size(), " objects"));
        }
    }
    if (!found)
    {
        return response.error("Unknown local_id");
    }
    LLLocalMeshSystem::getInstance()->applyVObject(obj->getID(), local_id, index, use_scale);
}

void LLBuildListener::localMeshReload(LLSD const& data)
{
    Response response(LLSD(), data);
    LLLocalMeshSystem::getInstance()->reloadFile(data["local_id"].asUUID());
}

void LLBuildListener::localMeshClear(LLSD const& data)
{
    Response response(LLSD(), data);
    LLViewerObject* obj = find_editable(response, data["id"], false);
    if (!obj) return;
    if (!obj->isMesh() || !obj->getSculptParams())
    {
        return response.error("Object is not showing a mesh");
    }
    LLLocalMeshSystem::getInstance()->clearVObject(obj->getID());
}

void LLBuildListener::localMeshRemove(LLSD const& data)
{
    Response response(LLSD(), data);
    LLLocalMeshSystem::getInstance()->deleteFile(data["local_id"].asUUID());
}

namespace
{
    // Sends one DeRezObject for the given root objects (all in one region).
    void send_derez(const std::vector<LLViewerObject*>& roots, U8 destination, const LLUUID& folder_id)
    {
        LLMessageSystem* msg = gMessageSystem;
        msg->newMessageFast(_PREHASH_DeRezObject);
        msg->nextBlockFast(_PREHASH_AgentData);
        msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        msg->nextBlockFast(_PREHASH_AgentBlock);
        msg->addUUIDFast(_PREHASH_GroupID, gAgent.getGroupID());
        msg->addU8Fast(_PREHASH_Destination, destination);
        msg->addUUIDFast(_PREHASH_DestinationID, folder_id);
        LLUUID tid;
        tid.generate();
        msg->addUUIDFast(_PREHASH_TransactionID, tid);
        msg->addU8Fast(_PREHASH_PacketCount, 1);
        msg->addU8Fast(_PREHASH_PacketNumber, 1);
        for (LLViewerObject* obj : roots)
        {
            msg->nextBlockFast(_PREHASH_ObjectData);
            msg->addU32Fast(_PREHASH_ObjectLocalID, obj->getLocalID());
        }
        msg->sendReliable(roots.front()->getRegion()->getHost());
    }

    bool owns_full_perm(const LLPermissions& perms)
    {
        const LLUUID& me = gAgent.getID();
        return perms.allowOperationBy(PERM_COPY, me) && perms.allowOperationBy(PERM_MODIFY, me)
            && perms.allowOperationBy(PERM_TRANSFER, me);
    }

    // Plain text <-> Linden notecard asset format.
    std::string notecard_from_text(const std::string& text)
    {
        LLNotecard notecard(LLNotecard::MAX_SIZE);
        notecard.setText(text);
        std::stringstream out;
        notecard.exportStream(out);
        return out.str();
    }

    std::string text_from_notecard(const char* buffer, S32 length)
    {
        if (length > 19 && !strncmp(buffer, "Linden text version", 19))
        {
            LLMemoryStream in((U8*)buffer, length);
            LLNotecard notecard(LLNotecard::MAX_SIZE);
            if (notecard.importStream(in))
            {
                return notecard.getText();
            }
        }
        return std::string(buffer, length); // version 0: plain text
    }

    void on_item_text_loaded(const LLUUID& asset_id, LLAssetType::EType type, void* user_data,
                             S32 status, LLExtStat ext_status)
    {
        std::unique_ptr<LLSD> request((LLSD*)user_data);
        if (status != LL_ERR_NOERR)
        {
            sendReply(llsd::map("error", stringize("Could not read the asset (status ", status,
                                                   status == LL_ERR_INSUFFICIENT_PERMISSIONS ? ", no permission" : "",
                                                   ")")), *request);
            return;
        }
        LLFileSystem file(asset_id, type, LLFileSystem::READ);
        S32 length = file.getSize();
        std::vector<char> buffer(length + 1, 0);
        if (length > 0)
        {
            file.read((U8*)buffer.data(), length);
        }
        while (length > 0 && buffer[length - 1] == '\0')
        {
            --length; // assets are stored with a trailing NUL
        }
        std::string text = (type == LLAssetType::AT_NOTECARD) ? text_from_notecard(buffer.data(), length)
                                                              : std::string(buffer.data(), length);
        sendReply(llsd::map("text", text), *request);
    }

    // Agent inventory items often arrive as summaries; a full copy (permissions, flags,
    // asset id) is needed before using one. Returns null and starts a fetch if it isn't
    // complete yet; the caller replies "loading" so the plugin can retry.
    LLViewerInventoryItem* complete_item(const LLUUID& item_id, std::string& problem)
    {
        LLViewerInventoryItem* item = gInventory.getItem(item_id);
        if (!item)
        {
            problem = "Inventory item not found";
            return nullptr;
        }
        if (!item->isFinished())
        {
            item->fetchFromServer();
            problem = "Inventory item is loading, try again in a moment";
            return nullptr;
        }
        return item;
    }

    // Gives a newly created selected object's id to the request (full-perm objects only).
    void reply_with_next_new_object(const LLSD& request)
    {
        gObjectList.setNewObjectCallback([request](LLViewerObject* obj) -> bool
        {
            sendReply(llsd::map("id", obj->getID(), "object", object_info(obj, false)), request);
            return false;
        });
    }
}

void LLBuildListener::rezFromInventory(LLSD const& data)
{
    LLViewerRegion* region = gAgent.getRegion();
    std::string problem;
    LLViewerInventoryItem* item = region ? complete_item(data["item_id"].asUUID(), problem) : nullptr;
    if (!item)
    {
        Response(LLSD(), data).error(region ? problem : "Not logged in to a region");
        return;
    }
    if (item->getType() != LLAssetType::AT_OBJECT)
    {
        Response(LLSD(), data).error("Only objects can be rezzed");
        return;
    }
    bool can_copy = item->getPermissions().allowCopyBy(gAgent.getID());
    if (!can_copy && !data["allow_move"].asBoolean())
    {
        Response(LLSD(), data).error("This item is no-copy: rezzing would take it out of the inventory "
                                     "(pass allow_move to accept)");
        return;
    }
    LLVector3 pos = sd_vec3(data["region_pos"]);
    bool full_perm = owns_full_perm(item->getPermissions());
    if (full_perm)
    {
        reply_with_next_new_object(data);
    }

    LLMessageSystem* msg = gMessageSystem;
    msg->newMessageFast(_PREHASH_RezObject);
    msg->nextBlockFast(_PREHASH_AgentData);
    msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
    msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
    msg->addUUIDFast(_PREHASH_GroupID, FSCommon::getGroupForRezzing());
    msg->nextBlock("RezData");
    msg->addUUIDFast(_PREHASH_FromTaskID, LLUUID::null);
    msg->addU8Fast(_PREHASH_BypassRaycast, (U8)1);
    msg->addVector3Fast(_PREHASH_RayStart, pos);
    msg->addVector3Fast(_PREHASH_RayEnd, pos);
    msg->addUUIDFast(_PREHASH_RayTargetID, LLUUID::null);
    msg->addBOOLFast(_PREHASH_RayEndIsIntersection, false);
    msg->addBOOLFast(_PREHASH_RezSelected, true);
    msg->addBOOLFast(_PREHASH_RemoveItem, !can_copy);
    const LLPermissions& perms = item->getPermissions();
    msg->addU32Fast(_PREHASH_ItemFlags, item->getFlags());
    msg->addU32Fast(_PREHASH_GroupMask, perms.getMaskGroup());
    msg->addU32Fast(_PREHASH_EveryoneMask, perms.getMaskEveryone());
    msg->addU32Fast(_PREHASH_NextOwnerMask, perms.getMaskNextOwner());
    msg->nextBlockFast(_PREHASH_InventoryData);
    item->packMessage(msg);
    msg->sendReliable(region->getHost());

    if (!full_perm)
    {
        Response(llsd::map("id", LLSD(), "note", "Not full-permission: look for it near the target"), data);
    }
}

void LLBuildListener::takeObjects(LLSD const& data)
{
    Response response(LLSD(), data);
    bool copy = data["copy"].asBoolean();
    std::vector<LLViewerObject*> roots;
    for (const LLSD& id : llsd::inArray(data["ids"]))
    {
        LLViewerObject* obj = find_editable(response, id, false);
        if (!obj || !obj->getRegion()) return;
        obj = obj->getRootEdit();
        if (copy ? !obj->permCopy() : !obj->permYouOwner())
        {
            return response.error(stringize(copy ? "No copy permission on " : "You do not own ", id.asString()));
        }
        if (!roots.empty() && roots.front()->getRegion() != obj->getRegion())
        {
            return response.error("All objects must be in the same region");
        }
        roots.push_back(obj);
    }
    if (roots.empty())
    {
        return response.error("No objects given");
    }
    send_derez(roots, copy ? DRD_ACQUIRE_TO_AGENT_INVENTORY : DRD_TAKE_INTO_AGENT_INVENTORY,
               gInventory.findCategoryUUIDForType(LLFolderType::FT_OBJECT));
}

void LLBuildListener::duplicateObject(LLSD const& data)
{
    LLViewerObject* obj = gObjectList.findObject(data["id"].asUUID());
    if (!obj || obj->isDead() || !obj->getRegion() || !obj->permCopy())
    {
        Response(LLSD(), data).error("Object not found or not copyable");
        return;
    }
    obj = obj->getRootEdit();
    // The copy's id can only be caught for objects we own with full permissions.
    bool full_perm = obj->permYouOwner() && obj->permModify() && obj->permTransfer();
    if (full_perm)
    {
        reply_with_next_new_object(data);
    }
    LLMessageSystem* msg = gMessageSystem;
    msg->newMessageFast(_PREHASH_ObjectDuplicate);
    msg->nextBlockFast(_PREHASH_AgentData);
    msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
    msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
    msg->addUUIDFast(_PREHASH_GroupID, FSCommon::getGroupForRezzing());
    msg->nextBlockFast(_PREHASH_SharedData);
    msg->addVector3Fast(_PREHASH_Offset, sd_vec3(data["offset"]));
    msg->addU32Fast(_PREHASH_DuplicateFlags, FLAGS_CREATE_SELECTED);
    msg->nextBlockFast(_PREHASH_ObjectData);
    msg->addU32Fast(_PREHASH_ObjectLocalID, obj->getLocalID());
    msg->sendReliable(obj->getRegion()->getHost());
    if (!full_perm)
    {
        Response(llsd::map("id", LLSD(), "note", "Not full-permission: look for the copy near the offset"), data);
    }
}

void LLBuildListener::addInventoryToObject(LLSD const& data)
{
    Response response(LLSD(), data);
    LLViewerObject* obj = find_editable(response, data["id"]);
    if (!obj) return;
    std::string problem;
    LLViewerInventoryItem* item = complete_item(data["item_id"].asUUID(), problem);
    if (!item)
    {
        return response.error(problem);
    }
    if (!item->getPermissions().allowCopyBy(gAgent.getID()) && !data["allow_move"].asBoolean())
    {
        return response.error("This item is no-copy: it would leave your inventory (pass allow_move to accept)");
    }
    LLPointer<LLViewerInventoryItem> copy = new LLViewerInventoryItem(item);
    copy->setCreationDate(time_corrected());
    if (item->getType() == LLAssetType::AT_LSL_TEXT)
    {
        obj->saveScript(copy, true, true);
    }
    else
    {
        obj->updateInventory(copy, TASK_INVENTORY_ITEM_KEY, true);
    }
}

void LLBuildListener::removeObjectInventory(LLSD const& data)
{
    Response response(LLSD(), data);
    LLViewerObject* obj = find_editable(response, data["id"]);
    if (!obj || !obj->getRegion()) return;
    obj->removeInventory(data["item_id"].asUUID());
}

void LLBuildListener::createNotecard(LLSD const& data)
{
    std::string url = gAgent.getRegion() ? gAgent.getRegion()->getCapability("UpdateNotecardAgentInventory") : "";
    if (url.empty())
    {
        Response(LLSD(), data).error("Region does not offer the UpdateNotecardAgentInventory capability");
        return;
    }
    LLSD request(data);
    std::string buffer = notecard_from_text(data["text"].asString());
    // 1) create an empty notecard in agent inventory, 2) upload its text,
    // 3) optionally copy it into the object and drop the inventory copy.
    LLPointer<LLInventoryCallback> created = new LLBoostFuncInventoryCallback(
        [request, buffer, url](const LLUUID& item_id)
        {
            LLResourceUploadInfo::ptr_t info = std::make_shared<LLBufferedAssetUploadInfo>(
                item_id, LLAssetType::AT_NOTECARD, buffer,
                [request](LLUUID item_id, LLUUID new_asset_id, LLUUID new_item_id, LLSD)
                {
                    LLUUID final_id = new_item_id.notNull() ? new_item_id : item_id;
                    LLViewerInventoryItem* item = gInventory.getItem(final_id);
                    if (item)
                    {
                        LLPointer<LLViewerInventoryItem> updated = new LLViewerInventoryItem(item);
                        updated->setAssetUUID(new_asset_id);
                        gInventory.updateItem(updated);
                        gInventory.notifyObservers();
                    }
                    LLSD reply = llsd::map("item_id", final_id, "asset_id", new_asset_id);
                    LLViewerObject* obj = request.has("id") ? gObjectList.findObject(request["id"].asUUID()) : nullptr;
                    if (obj && item)
                    {
                        LLPointer<LLViewerInventoryItem> copy = new LLViewerInventoryItem(item);
                        copy->setAssetUUID(new_asset_id);
                        obj->updateInventory(copy, TASK_INVENTORY_ITEM_KEY, true);
                        reply["copied_to"] = obj->getID();
                        if (!request["keep_copy"].asBoolean())
                        {
                            // Give the simulator time to copy it before removing ours.
                            doAfterInterval([final_id]() { remove_inventory_item(final_id, nullptr); }, 5.f);
                        }
                    }
                    sendReply(reply, request);
                },
                [request](LLUUID, LLUUID, LLSD, std::string reason) -> bool
                {
                    sendReply(llsd::map("error", "Notecard upload failed: " + reason), request);
                    return false;
                });
            LLViewerAssetUpload::EnqueueInventoryUpload(url, info);
        });
    create_inventory_item(gAgent.getID(), gAgent.getSessionID(),
                          gInventory.findCategoryUUIDForType(LLFolderType::FT_NOTECARD),
                          LLTransactionID::tnull, data["name"].asString(), "",
                          LLAssetType::AT_NOTECARD, LLInventoryType::IT_NOTECARD, 0,
                          LLFloaterPerms::getNextOwnerPerms("Notecards"), created);
}

void LLBuildListener::uploadNotecard(LLSD const& data)
{
    LLSD request(data);
    std::string buffer = notecard_from_text(data["text"].asString());
    LLUUID item_id = data["item_id"].asUUID();
    auto failed = [request](LLUUID, LLUUID, LLSD, std::string reason) -> bool
    {
        sendReply(llsd::map("error", "Notecard upload failed: " + reason), request);
        return false;
    };
    if (data.has("id"))
    {
        LLViewerObject* obj = gObjectList.findObject(data["id"].asUUID());
        std::string url = (obj && obj->getRegion()) ? obj->getRegion()->getCapability("UpdateNotecardTaskInventory") : "";
        if (url.empty() || !obj->permModify())
        {
            Response(LLSD(), data).error("Object not found, not modifiable, or region lacks UpdateNotecardTaskInventory");
            return;
        }
        LLViewerAssetUpload::EnqueueInventoryUpload(url, std::make_shared<LLBufferedAssetUploadInfo>(
            obj->getID(), item_id, LLAssetType::AT_NOTECARD, buffer,
            [request](LLUUID, LLUUID, LLUUID new_asset_id, LLSD)
            { sendReply(llsd::map("asset_id", new_asset_id), request); },
            failed));
        return;
    }
    std::string url = gAgent.getRegion() ? gAgent.getRegion()->getCapability("UpdateNotecardAgentInventory") : "";
    if (url.empty() || !gInventory.getItem(item_id))
    {
        Response(LLSD(), data).error("Notecard not found, or region lacks UpdateNotecardAgentInventory");
        return;
    }
    LLViewerAssetUpload::EnqueueInventoryUpload(url, std::make_shared<LLBufferedAssetUploadInfo>(
        item_id, LLAssetType::AT_NOTECARD, buffer,
        [request](LLUUID item_id, LLUUID new_asset_id, LLUUID new_item_id, LLSD)
        {
            if (LLViewerInventoryItem* item = gInventory.getItem(new_item_id.notNull() ? new_item_id : item_id))
            {
                LLPointer<LLViewerInventoryItem> updated = new LLViewerInventoryItem(item);
                updated->setAssetUUID(new_asset_id);
                gInventory.updateItem(updated);
                gInventory.notifyObservers();
            }
            sendReply(llsd::map("asset_id", new_asset_id), request);
        },
        failed));
}

void LLBuildListener::readItemText(LLSD const& data)
{
    LLViewerInventoryItem* item = nullptr;
    LLHost host;
    LLUUID task_id;
    if (data.has("id"))
    {
        LLViewerObject* obj = gObjectList.findObject(data["id"].asUUID());
        if (!obj || !obj->getRegion())
        {
            Response(LLSD(), data).error("Object not found");
            return;
        }
        item = dynamic_cast<LLViewerInventoryItem*>(obj->getInventoryObject(data["item_id"].asUUID()));
        host = obj->getRegion()->getHost();
        task_id = obj->getID();
        if (!item)
        {
            Response(LLSD(), data).error("Item not in the object's fetched contents (list them first)");
            return;
        }
    }
    else
    {
        std::string problem;
        item = complete_item(data["item_id"].asUUID(), problem);
        if (!item)
        {
            Response(LLSD(), data).error(problem);
            return;
        }
    }
    if (item->getType() != LLAssetType::AT_LSL_TEXT && item->getType() != LLAssetType::AT_NOTECARD)
    {
        Response(LLSD(), data).error("Only scripts and notecards can be read");
        return;
    }
    // Inside objects the simulator hides script asset ids (they arrive null) but still serves
    // the asset by task + item id, as the script editor does. In agent inventory a null
    // asset id means the item was never saved.
    if (item->getAssetUUID().isNull() && task_id.isNull())
    {
        Response(llsd::map("text", ""), data);
        return;
    }
    gAssetStorage->getInvItemAsset(host, gAgent.getID(), gAgent.getSessionID(),
                                   item->getPermissions().getOwner(), task_id, item->getUUID(),
                                   item->getAssetUUID(), item->getType(),
                                   &on_item_text_loaded, new LLSD(data), true);
}

void LLBuildListener::resetScript(LLSD const& data)
{
    Response response(LLSD(), data);
    LLViewerObject* obj = find_editable(response, data["id"]);
    if (!obj || !obj->getRegion()) return;
    LLMessageSystem* msg = gMessageSystem;
    msg->newMessageFast(_PREHASH_ScriptReset);
    msg->nextBlockFast(_PREHASH_AgentData);
    msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
    msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
    msg->nextBlockFast(_PREHASH_Script);
    msg->addUUIDFast(_PREHASH_ObjectID, obj->getID());
    msg->addUUIDFast(_PREHASH_ItemID, data["item_id"].asUUID());
    msg->sendReliable(obj->getRegion()->getHost());
}

void LLBuildListener::getObjectCosts(LLSD const& data)
{
    Response response(LLSD(), data);
    LLViewerObject* obj = find_editable(response, data["id"], false);
    if (!obj) return;
    LLViewerObject* root = obj->getRootEdit();
    // Each getter queues a GetObjectCost fetch while its value is stale.
    response["land_impact"] = root->getLinksetCost();
    response["linkset_physics_cost"] = root->getLinksetPhysicsCost();
    response["prim_cost"] = obj->getObjectCost();
    response["prim_physics_cost"] = obj->getPhysicsCost();
    response["download_weight"] = obj->getStreamingCost();
    response["root_id"] = root->getID();
}

void LLBuildListener::getParcelInfo(LLSD const& data)
{
    Response response(LLSD(), data);
    LLParcel* parcel = LLViewerParcelMgr::getInstance()->getAgentParcel();
    if (!parcel)
    {
        return response.error("Parcel information not available yet");
    }
    response["name"] = parcel->getName();
    response["area"] = parcel->getArea();
    response["prims_used"] = parcel->getPrimCount();
    response["prims_max"] = ll_round(parcel->getMaxPrimCapacity() * parcel->getParcelPrimBonus());
    response["region_prims_used"] = parcel->getSimWidePrimCount();
    response["region_prims_max"] = parcel->getSimWideMaxPrimCapacity();
    response["build_allowed"] = LLViewerParcelMgr::getInstance()->allowAgentBuild();

    // <FS:MCP> Where the parcel is, so a build can be planned to fit inside it.
    const LLVector3& lo = parcel->getAABBMin();
    const LLVector3& hi = parcel->getAABBMax();
    response["bounds_min"] = llsd::array(lo.mV[VX], lo.mV[VY]);
    response["bounds_max"] = llsd::array(hi.mV[VX], hi.mV[VY]);
    F32 box_area = (hi.mV[VX] - lo.mV[VX]) * (hi.mV[VY] - lo.mV[VY]);
    bool rectangular = box_area > 0.f && llabs(box_area - (F32)parcel->getArea()) < 1.f;
    response["rectangular"] = rectangular;
    LLViewerRegion* region = gAgent.getRegion();
    if (!rectangular && region)
    {
        // Irregular parcel: one character per 4x4 m land cell, '#' = this parcel, rows from
        // north (bounds_max y) to south, columns from west (bounds_min x) to east.
        LLViewerParcelMgr* parcels = LLViewerParcelMgr::getInstance();
        LLSD rows = LLSD::emptyArray();
        for (F32 y = hi.mV[VY] - 2.f; y > lo.mV[VY]; y -= 4.f)
        {
            std::string row;
            for (F32 x = lo.mV[VX] + 2.f; x < hi.mV[VX]; x += 4.f)
            {
                LLVector3 cell(x, y, 0.f);
                row += parcels->inAgentParcel(region->getPosGlobalFromRegion(cell)) ? '#' : '.';
            }
            rows.append(row);
        }
        response["cells_4m"] = rows;
    }
}

void LLBuildListener::canBuildAt(LLSD const& data)
{
    Response response(LLSD(), data);
    LLViewerRegion* region = gAgent.getRegion();
    if (!region)
    {
        return response.error("Not logged in to a region");
    }
    LLVector3 pos = sd_vec3(data["region_pos"]);
    if (pos.mV[VX] < 0.f || pos.mV[VY] < 0.f || pos.mV[VX] >= region->getWidth() || pos.mV[VY] >= region->getWidth())
    {
        response["allowed"] = false;
        response["reason"] = "Position is outside the avatar's region";
        return;
    }
    LLViewerParcelMgr* parcels = LLViewerParcelMgr::getInstance();
    bool in_agent_parcel = parcels->inAgentParcel(region->getPosGlobalFromRegion(pos));
    response["in_agent_parcel"] = in_agent_parcel;
    if (in_agent_parcel)
    {
        // Full answer: owner, group members with "create objects", or land open to all.
        bool allowed = parcels->allowAgentBuild();
        response["allowed"] = allowed;
        LLParcel* parcel = parcels->getAgentParcel();
        response["parcel"] = parcel ? parcel->getName() : std::string();
        response["reason"] = allowed ? "Building is allowed on this parcel"
                                     : "This parcel does not let you create objects";
        return;
    }
    // Other parcels: the overlay only says who owns the land.
    U8 owner = region->getParcelOverlay() ? region->getParcelOverlay()->ownership(pos) : PARCEL_OWNED;
    if (owner == PARCEL_SELF)
    {
        response["allowed"] = true;
        response["reason"] = "Your own land";
    }
    else
    {
        response["reason"] = "Another parcel: its build rules are only known with the avatar on it";
    }
}

void LLBuildListener::setRenderMaterial(LLSD const& data)
{
    Response response(LLSD(), data);
    LLViewerObject* obj = find_editable(response, data["id"]);
    if (!obj) return;
    S32 face = data.has("face") ? data["face"].asInteger() : -1;
    if (face >= (S32)obj->getNumTEs())
    {
        return response.error(stringize("Object has only ", (S32)obj->getNumTEs(), " faces"));
    }
    obj->setRenderMaterialID(face < 0 ? -1 : face, data["material_id"].asUUID(), true);
}

void LLBuildListener::setExtraParams(LLSD const& data)
{
    Response response(LLSD(), data);
    LLViewerObject* obj = find_editable(response, data["id"]);
    if (!obj || !obj->getRegion()) return;
    LLVOVolume* vol = dynamic_cast<LLVOVolume*>(obj);
    if (!vol)
    {
        return response.error("Object is not a prim");
    }

    if (data.has("light"))
    {
        const LLSD& light = data["light"];
        bool on = light.isMap() || light.asBoolean();
        vol->setIsLight(on);
        if (light.isMap())
        {
            if (light.has("color"))
            {
                vol->setLightSRGBColor(LLColor3((F32)light["color"][0].asReal(), (F32)light["color"][1].asReal(),
                                                (F32)light["color"][2].asReal()));
            }
            if (light.has("intensity")) vol->setLightIntensity(llclamp((F32)light["intensity"].asReal(), 0.f, 1.f));
            if (light.has("radius"))    vol->setLightRadius(llclamp((F32)light["radius"].asReal(), 0.f, 20.f));
            if (light.has("falloff"))   vol->setLightFalloff(llclamp((F32)light["falloff"].asReal(), 0.f, 2.f));
        }
    }

    if (data.has("flexible"))
    {
        const LLSD& flex = data["flexible"];
        bool on = flex.isMap() || flex.asBoolean();
        if (vol->setIsFlexible(on))
        {
            obj->sendShapeUpdate();
        }
        if (on && flex.isMap())
        {
            if (const LLFlexibleObjectData* current = obj->getFlexibleObjectData())
            {
                LLFlexibleObjectData attrs = *current;
                if (flex.has("softness")) attrs.setSimulateLOD(llclamp(flex["softness"].asInteger(), 0, 3));
                if (flex.has("gravity"))  attrs.setGravity((F32)flex["gravity"].asReal());
                if (flex.has("drag"))     attrs.setAirFriction((F32)flex["drag"].asReal());
                if (flex.has("wind"))     attrs.setWindSensitivity((F32)flex["wind"].asReal());
                if (flex.has("tension"))  attrs.setTension((F32)flex["tension"].asReal());
                if (flex.has("force"))
                {
                    LLVector3 force = sd_vec3(flex["force"]);
                    attrs.setUserForce(force);
                }
                obj->setParameterEntry(LLNetworkData::PARAMS_FLEXIBLE, attrs, true);
            }
        }
    }

    if (data.has("physics_shape"))
    {
        std::string shape = data["physics_shape"].asString();
        U8 type = shape == "none" ? LLViewerObject::PHYSICS_SHAPE_NONE
                : shape == "convex" ? LLViewerObject::PHYSICS_SHAPE_CONVEX_HULL
                : LLViewerObject::PHYSICS_SHAPE_PRIM;
        obj->setPhysicsShapeType(type);
        obj->updateFlags(true);
    }

    if (data.has("click_action"))
    {
        static const std::map<std::string, U8> actions = {
            { "touch", CLICK_ACTION_TOUCH }, { "none", CLICK_ACTION_NONE }, { "sit", CLICK_ACTION_SIT },
            { "buy", CLICK_ACTION_BUY }, { "pay", CLICK_ACTION_PAY }, { "open", CLICK_ACTION_OPEN },
            { "play", CLICK_ACTION_PLAY }, { "open_media", CLICK_ACTION_OPEN_MEDIA },
            { "zoom", CLICK_ACTION_ZOOM }, { "disabled", CLICK_ACTION_DISABLED } };
        auto found = actions.find(data["click_action"].asString());
        if (found == actions.end())
        {
            return response.error("Unknown click_action");
        }
        obj->setClickAction(found->second);
        begin_agent_message("ObjectClickAction");
        gMessageSystem->nextBlockFast(_PREHASH_ObjectData);
        gMessageSystem->addU32Fast(_PREHASH_ObjectLocalID, obj->getLocalID());
        gMessageSystem->addU8("ClickAction", found->second);
        gMessageSystem->sendReliable(obj->getRegion()->getHost());
    }

    if (data.has("sculpt"))
    {
        static const std::map<std::string, U8> types = {
            { "sphere", LL_SCULPT_TYPE_SPHERE }, { "torus", LL_SCULPT_TYPE_TORUS },
            { "plane", LL_SCULPT_TYPE_PLANE }, { "cylinder", LL_SCULPT_TYPE_CYLINDER } };
        auto found = types.find(data["sculpt"]["type"].asString());
        U8 type = found != types.end() ? found->second : LL_SCULPT_TYPE_SPHERE;
        LLUUID texture = data["sculpt"]["texture"].asUUID();
        LLSculptParams sculpt;
        sculpt.setSculptTexture(texture, type);
        obj->setParameterEntry(LLNetworkData::PARAMS_SCULPT, sculpt, true);
        LLVolumeParams params;
        params.setType(LL_PCODE_PROFILE_CIRCLE, LL_PCODE_PATH_CIRCLE);
        params.setSculptID(texture, type);
        obj->updateVolume(params);
    }
}

void LLBuildListener::uploadFile(LLSD const& data)
{
    Response response(LLSD(), data);
    std::string path = data["path"].asString();
    std::string ext = gDirUtilp->getExtension(path);
    if (ext == "bvh")
    {
        // BVH needs the preview window (priority, ease, loop); the user finishes the upload.
        LLFloaterReg::showInstance("upload_anim_bvh", LLSD(path));
        response["opened_window"] = true;
        return;
    }
    LLAssetType::EType asset_type;
    U32 codec;
    if (!LLResourceUploadInfo::findAssetTypeAndCodecOfExtension(ext, asset_type, codec, true))
    {
        return response.error("Unsupported file type: " + ext);
    }
    const LLAgentBenefits& benefits = LLAgentBenefitsMgr::current();
    S32 cost = 0;
    switch (asset_type)
    {
    case LLAssetType::AT_TEXTURE:   cost = benefits.getTextureUploadCost(); break;
    case LLAssetType::AT_SOUND:     cost = benefits.getSoundUploadCost(); break;
    case LLAssetType::AT_ANIMATION: cost = benefits.getAnimationUploadCost(); break;
    default:
        return response.error("Only textures, sounds and .anim animations can be uploaded this way");
    }
    response["expected_cost"] = cost;
    if (cost > data["max_cost"].asInteger())
    {
        return response.error(stringize("This upload costs L$", cost, "; pass max_cost >= ", cost,
                                        " if the user agreed to pay"));
    }
    std::string name = data.has("name") ? data["name"].asString() : gDirUtilp->getBaseFileName(path, true);
    LLResourceUploadInfo::ptr_t info = std::make_shared<LLNewFileResourceUploadInfo>(
        path, name, data["desc"].asString(), 0, LLFolderType::FT_NONE, LLInventoryType::IT_NONE,
        LLFloaterPerms::getNextOwnerPerms("Uploads"), LLFloaterPerms::getGroupPerms("Uploads"),
        LLFloaterPerms::getEveryonePerms("Uploads"), cost);
    upload_new_resource(info);
    response["queued"] = true;
    response["name"] = name;
}

// static
void LLBuildListener::processObjectPropertiesFamily(LLMessageSystem* msg)
{
    LLUUID id, owner_id, group_id, last_owner_id;
    std::string name, desc;
    msg->getUUIDFast(_PREHASH_ObjectData, _PREHASH_ObjectID, id);
    msg->getUUIDFast(_PREHASH_ObjectData, _PREHASH_OwnerID, owner_id);
    msg->getUUIDFast(_PREHASH_ObjectData, _PREHASH_GroupID, group_id);
    msg->getUUIDFast(_PREHASH_ObjectData, _PREHASH_LastOwnerID, last_owner_id);
    msg->getStringFast(_PREHASH_ObjectData, _PREHASH_Name, name);
    msg->getStringFast(_PREHASH_ObjectData, _PREHASH_Description, desc);
    if (sObjectProps.size() >= OBJECT_PROPS_MAX && !sObjectProps.count(id))
    {
        sObjectProps.erase(sObjectProps.begin());
    }
    U32 owner_mask = 0, group_mask = 0, everyone_mask = 0, next_owner_mask = 0;
    U8 sale_type = 0;
    S32 sale_price = 0;
    msg->getU32Fast(_PREHASH_ObjectData, _PREHASH_OwnerMask, owner_mask);
    msg->getU32Fast(_PREHASH_ObjectData, _PREHASH_GroupMask, group_mask);
    msg->getU32Fast(_PREHASH_ObjectData, _PREHASH_EveryoneMask, everyone_mask);
    msg->getU32Fast(_PREHASH_ObjectData, _PREHASH_NextOwnerMask, next_owner_mask);
    msg->getU8Fast(_PREHASH_ObjectData, _PREHASH_SaleType, sale_type);
    msg->getS32Fast(_PREHASH_ObjectData, _PREHASH_SalePrice, sale_price);
    sObjectProps[id] = llsd::map("name", name, "desc", desc, "owner_id", owner_id,
                                 "group_id", group_id, "last_owner_id", last_owner_id,
                                 "permissions", llsd::map(
                                     "owner", perm_mask_sd(owner_mask),
                                     "group", perm_mask_sd(group_mask),
                                     "everyone", perm_mask_sd(everyone_mask),
                                     "next_owner", perm_mask_sd(next_owner_mask),
                                     "owner_id", owner_id),
                                 "sale", sale_sd(sale_type, sale_price),
                                 "seq", ++sObjectPropsSeq);
}

// static
LLSD LLBuildListener::cachedObjectProperties(const LLUUID& id)
{
    auto found = sObjectProps.find(id);
    return found != sObjectProps.end() ? found->second : LLSD();
}

void LLBuildListener::requestObjectProperties(LLSD const& data)
{
    Response response(LLSD(), data);
    S32 sent = 0;
    for (const LLSD& id : llsd::inArray(data["ids"]))
    {
        LLViewerObject* obj = gObjectList.findObject(id.asUUID());
        if (!obj || obj->isDead() || !obj->getRegion()) continue;
        LLMessageSystem* msg = gMessageSystem;
        msg->newMessageFast(_PREHASH_RequestObjectPropertiesFamily);
        msg->nextBlockFast(_PREHASH_AgentData);
        msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        msg->nextBlockFast(_PREHASH_ObjectData);
        msg->addU32Fast(_PREHASH_RequestFlags, 0x0);
        msg->addUUIDFast(_PREHASH_ObjectID, obj->getID());
        msg->sendReliable(obj->getRegion()->getHost());
        ++sent;
    }
    response["requested"] = sent;
}

void LLBuildListener::getObjectProperties(LLSD const& data)
{
    Response response(LLSD(), data);
    LLSD objects = LLSD::emptyMap();
    LLSD missing = LLSD::emptyArray();
    for (const LLSD& id : llsd::inArray(data["ids"]))
    {
        LLSD props = cachedObjectProperties(id.asUUID());
        if (props.isUndefined())
        {
            missing.append(id);
            continue;
        }
        LLAvatarName av_name;
        if (LLAvatarNameCache::get(props["owner_id"].asUUID(), &av_name))
        {
            props["owner_name"] = av_name.getUserName();
        }
        objects[id.asString()] = props;
    }
    response["objects"] = objects;
    response["missing"] = missing;
}

void LLBuildListener::getWornItems(LLSD const& data)
{
    Response response(LLSD(), data);
    LLSD wearables = LLSD::emptyArray();
    LLWearableType* types = LLWearableType::getInstance();
    for (S32 t = 0; t < LLWearableType::WT_COUNT; ++t)
    {
        LLWearableType::EType type = (LLWearableType::EType)t;
        for (U32 i = 0; i < gAgentWearables.getWearableCount(type); ++i)
        {
            LLUUID item_id = gAgentWearables.getWearableItemID(type, i);
            LLViewerInventoryItem* item = gInventory.getItem(item_id);
            wearables.append(llsd::map(
                "type", types->getTypeName(type),
                "layer", LLSD::Integer(i),
                "item_id", item_id,
                "name", item ? item->getName() : std::string()));
        }
    }
    LLSD attachments = LLSD::emptyArray();
    if (isAgentAvatarValid())
    {
        for (const auto& [index, point] : gAgentAvatarp->mAttachmentPoints)
        {
            if (!point) continue;
            for (const auto& attached : point->mAttachedObjects)
            {
                if (LLViewerObject* obj = attached.get())
                {
                    attachments.append(llsd::map(
                        "object_id", obj->getID(),
                        "item_id", obj->getAttachmentItemID(),
                        "name", obj->getAttachmentItemName(),
                        "attachment_point", point->getName(),
                        "is_temporary", obj->isTempAttachment()));
                }
            }
        }
    }
    response["wearables"] = wearables;
    response["attachments"] = attachments;
}

void LLBuildListener::getAvatarPose(LLSD const& data)
{
    Response response(LLSD(), data);
    LLViewerRegion* region = gAgent.getRegion();
    if (!region || !isAgentAvatarValid())
    {
        return response.error("Avatar not available");
    }
    static const std::pair<const char*, const char*> joints[] = {
        { "pelvis", "mPelvis" }, { "torso", "mTorso" }, { "chest", "mChest" }, { "head", "mHead" },
        { "hip_left", "mHipLeft" }, { "hip_right", "mHipRight" },
        { "knee_left", "mKneeLeft" }, { "knee_right", "mKneeRight" },
        { "ankle_left", "mAnkleLeft" }, { "ankle_right", "mAnkleRight" },
        { "foot_left", "mFootLeft" }, { "foot_right", "mFootRight" } };
    LLVector3 origin = region->getOriginAgent();
    LLSD out;
    for (const auto& [key, joint_name] : joints)
    {
        if (LLJoint* joint = gAgentAvatarp->getJoint(joint_name))
        {
            out[key] = vec3_sd(joint->getWorldPosition() - origin);
        }
    }
    response["joints"] = out;
    response["sitting"] = gAgentAvatarp->isSitting();
    response["avatar_rot_deg"] = euler_deg_sd(gAgentAvatarp->getRotationRegion());
    if (LLViewerObject* seat = dynamic_cast<LLViewerObject*>(gAgentAvatarp->getParent()))
    {
        response["sit_root_id"] = seat->getRootEdit()->getID();
    }
}

void LLBuildListener::snapshot(LLSD const& data)
{
    LLViewerRegion* region = gAgent.getRegion();
    if (!region)
    {
        Response(LLSD(), data).error("Not logged in to a region");
        return;
    }
    F32 settle = 1.f;
    if (data.has("camera_pos") && data.has("focus"))
    {
        LLVector3d cam = region->getPosGlobalFromRegion(sd_vec3(data["camera_pos"]));
        LLVector3d focus = region->getPosGlobalFromRegion(sd_vec3(data["focus"]));
        // Free the camera from the avatar first (as Alt+click does), or it snaps back.
        gAgentCamera.setFocusOnAvatar(false, false);
        gAgentCamera.setCameraPosAndFocusGlobal(cam, focus, data["object_id"].asUUID());
        gAgentCamera.setCameraAnimating(false);
    }
    if (data.has("settle"))
    {
        settle = llclamp((F32)data["settle"].asReal(), 0.f, 10.f);
    }
    LLSD request(data);
    // Give the camera and object LODs a moment, then render a frame without UI to disk.
    doAfterInterval([request]()
    {
        std::string path = request["path"].asString();
        S32 width = request.has("width") ? request["width"].asInteger() : 1024;
        S32 height = request.has("height") ? request["height"].asInteger() : 768;
        // show_ui also draws hover text (llSetText), which lives in the UI pass.
        bool ok = gViewerWindow->saveSnapshot(path, width, height,
                                              request["show_ui"].asBoolean(), request["show_hud"].asBoolean(),
                                              true /* do_rebuild */, false /* show_balance */,
                                              LLSnapshotModel::SNAPSHOT_TYPE_COLOR,
                                              LLSnapshotModel::SNAPSHOT_FORMAT_PNG);
        if (ok)
        {
            sendReply(llsd::map("path", path), request);
        }
        else
        {
            sendReply(llsd::map("error", "Could not capture the snapshot"), request);
        }
    }, settle);
}

void LLBuildListener::resetCamera(LLSD const& data)
{
    Response response(LLSD(), data);
    gAgentCamera.resetView(true, true);
}

void LLBuildListener::openMeshUpload(LLSD const& data)
{
    Response response(LLSD(), data);
    auto* fmp = (LLFloaterModelPreview*)LLFloaterReg::getInstance("upload_model");
    if (!fmp)
    {
        return response.error("Could not open the mesh upload window");
    }
    if (fmp->isModelLoading())
    {
        return response.error("The mesh upload window is busy loading a model");
    }
    fmp->setUploadDestination(LLUUID::null);
    fmp->loadHighLodModelFile(data["path"].asString());
}

// ---- <FS:MCP> permissions, sale, wearing, IM, teleport, export, undo support -------

void LLBuildListener::setPermissions(LLSD const& data)
{
    Response response(LLSD(), data);
    LLViewerObject* obj = find_editable(response, data["id"]);
    if (!obj || !obj->getRegion()) return;
    LLViewerObject* root = obj->getRootEdit();

    struct Change { const char* key; U8 field; U32 mask; };
    static const Change changes[] = {
        { "next_owner_copy",     PERM_NEXT_OWNER, PERM_COPY },
        { "next_owner_modify",   PERM_NEXT_OWNER, PERM_MODIFY },
        { "next_owner_transfer", PERM_NEXT_OWNER, PERM_TRANSFER },
        { "everyone_copy",       PERM_EVERYONE,   PERM_COPY },
        { "everyone_move",       PERM_EVERYONE,   PERM_MOVE },
        { "group_share",         PERM_GROUP,      PERM_MODIFY | PERM_MOVE | PERM_COPY },
    };
    bool any = false;
    for (const Change& change : changes)
    {
        if (!data.has(change.key))
            continue;
        // Same message the build floater sends; the simulator applies it to the whole linkset.
        begin_agent_message("ObjectPermissions");
        gMessageSystem->nextBlockFast(_PREHASH_HeaderData);
        gMessageSystem->addBOOLFast(_PREHASH_Override, false);
        gMessageSystem->nextBlockFast(_PREHASH_ObjectData);
        gMessageSystem->addU32Fast(_PREHASH_ObjectLocalID, root->getLocalID());
        gMessageSystem->addU8Fast(_PREHASH_Field, change.field);
        gMessageSystem->addBOOLFast(_PREHASH_Set, data[change.key].asBoolean());
        gMessageSystem->addU32Fast(_PREHASH_Mask, change.mask);
        gMessageSystem->sendReliable(root->getRegion()->getHost());
        any = true;
    }
    if (!any)
    {
        response.error("No permission change given");
    }
}

void LLBuildListener::setSaleInfo(LLSD const& data)
{
    Response response(LLSD(), data);
    LLViewerObject* obj = find_editable(response, data["id"], false);
    if (!obj || !obj->getRegion()) return;
    if (!obj->permYouOwner())
    {
        return response.error("Only the owner can sell an object");
    }
    const std::string type = data["type"].asString();
    LLSaleInfo::EForSale sale_type;
    if (type == "not")            sale_type = LLSaleInfo::FS_NOT;
    else if (type == "original")  sale_type = LLSaleInfo::FS_ORIGINAL;
    else if (type == "copy")      sale_type = LLSaleInfo::FS_COPY;
    else if (type == "contents")  sale_type = LLSaleInfo::FS_CONTENTS;
    else return response.error(stringize("Unknown sale type ", std::quoted(type)));

    S32 price = data["price"].asInteger();
    if (sale_type != LLSaleInfo::FS_NOT && price < 0)
    {
        return response.error("Price must be 0 or more");
    }
    LLSaleInfo sale_info(sale_type, sale_type == LLSaleInfo::FS_NOT ? 0 : price);
    LLViewerObject* root = obj->getRootEdit();
    begin_agent_message("ObjectSaleInfo");
    gMessageSystem->nextBlockFast(_PREHASH_ObjectData);
    gMessageSystem->addU32Fast(_PREHASH_LocalID, root->getLocalID());
    sale_info.packMessage(gMessageSystem);
    gMessageSystem->sendReliable(root->getRegion()->getHost());
}

void LLBuildListener::wearItem(LLSD const& data)
{
    Response response(LLSD(), data);
    LLViewerInventoryItem* item = gInventory.getItem(data["item_id"].asUUID());
    if (!item)
    {
        return response.error(stringize("Inventory item ", data["item_id"].asString(), " not found"));
    }
    bool replace = data["replace"].asBoolean();
    if (data.has("attach_point") && item->getType() == LLAssetType::AT_OBJECT)
    {
        if (!isAgentAvatarValid())
        {
            return response.error("Avatar not ready");
        }
        const std::string wanted = data["attach_point"].asString();
        for (const auto& [index, attachment] : gAgentAvatarp->mAttachmentPoints)
        {
            if (attachment && LLStringUtil::compareInsensitive(attachment->getName(), wanted) == 0)
            {
                rez_attachment(item, attachment, replace);
                response["attach_point"] = attachment->getName();
                return;
            }
        }
        return response.error(stringize("Unknown attachment point ", std::quoted(wanted)));
    }
    LLAppearanceMgr::instance().wearItemOnAvatar(item->getUUID(), true, replace);
}

void LLBuildListener::removeItem(LLSD const& data)
{
    Response response(LLSD(), data);
    const LLUUID item_id = data["item_id"].asUUID();
    if (!gInventory.getItem(item_id))
    {
        return response.error(stringize("Inventory item ", item_id.asString(), " not found"));
    }
    if (!get_is_item_worn(item_id))
    {
        return response.error("That item is not being worn");
    }
    LLAppearanceMgr::instance().removeItemFromAvatar(item_id);
}

void LLBuildListener::getIM(LLSD const& data)
{
    Response response(LLSD(), data);
    S32 max_messages = data.has("max_messages") ? data["max_messages"].asInteger() : 20;
    static const char* type_names[] = { "p2p", "group", "adhoc", "avaline", "none" };
    LLSD sessions = LLSD::emptyArray();
    for (const auto& [session_id, session] : LLIMModel::getInstance()->mId2SessionMap)
    {
        if (!session)
            continue;
        LLSD messages = LLSD::emptyArray();
        S32 count = 0;
        for (const LLSD& msg : session->mMsgs) // newest first
        {
            if (count++ >= max_messages)
                break;
            messages.append(llsd::map("from", msg["from"], "from_id", msg["from_id"],
                                      "message", msg["message"], "time", msg["time"]));
        }
        S32 type = (S32)session->mSessionType;
        sessions.append(llsd::map(
            "session_id", session_id,
            "name", session->mName,
            "type", (type >= 0 && type < 5) ? type_names[type] : "unknown",
            "unread", session->mNumUnread,
            "messages", messages));
    }
    response["sessions"] = sessions;
}

void LLBuildListener::confirmTeleport(LLSD const& data)
{
    const std::string region = data["region"].asString();
    const F64 x = data["x"].asReal(), y = data["y"].asReal(), z = data["z"].asReal();
    if (region.empty())
    {
        Response(LLSD(), data).error("Need a region name");
        return;
    }
    LLSD args;
    args["MESSAGE"] = stringize("O Claude (MCP) quer teleportar voce para ", region,
                                " (", llround((F32)x), ", ", llround((F32)y), ", ", llround((F32)z), "). Permitir?");
    LLSD request(data);
    LLNotificationsUtil::add("GenericAlertYesCancel", args, LLSD(),
        [request, region, x, y, z](const LLSD& notification, const LLSD& answer)
        {
            bool accepted = LLNotificationsUtil::getSelectedOption(notification, answer) == 0;
            if (accepted)
            {
                LLEventPumps::instance().obtain("LLTeleportHandler").post(
                    llsd::map("op", "teleport", "regionname", region, "x", x, "y", y, "z", z));
            }
            sendReply(llsd::map("accepted", accepted), request);
        });
}

void LLBuildListener::openExport(LLSD const& data)
{
    Response response(LLSD(), data);
    LLViewerObject* obj = find_editable(response, data["id"], false);
    if (!obj) return;
    LLSelectMgr::getInstance()->deselectAll();
    LLSelectMgr::getInstance()->selectObjectAndFamily(obj->getRootEdit(), true);
    LLFloaterReg::showInstance("fs_export");
}

void LLBuildListener::setVolumeParams(LLSD const& data)
{
    Response response(LLSD(), data);
    LLViewerObject* obj = find_editable(response, data["id"]);
    if (!obj) return;
    if (obj->getPCode() != LL_PCODE_VOLUME || !obj->getVolume())
    {
        return response.error("Object is not a prim");
    }
    LLVolumeParams params;
    LLSD params_sd(data["params"]);
    if (!params.fromLLSD(params_sd))
    {
        return response.error("Invalid volume params");
    }
    obj->updateVolume(params);
}

// ---- <FS:MCP> HUDs, touching, script dialogs ----------------------------------------

namespace
{
    // Script dialogs (llDialog / llTextBox) currently shown, tracked from the "Visible" channel.
    std::set<LLUUID> sScriptDialogs;
    LLBoundListener sScriptDialogConnection;

    bool is_permission_request(const LLNotificationPtr& n)
    {
        return n && (n->getName() == "ScriptQuestion" || n->getName() == "ScriptQuestionCaution"
                     || n->getName() == "ScriptQuestionExperience");
    }

    // llDialog / llTextBox menus, and llRequestPermissions questions ("would like to: ...").
    bool is_script_dialog(const LLNotificationPtr& n)
    {
        return n && (n->getName() == "ScriptDialog" || n->getName() == "ScriptDialogGroup"
                     || is_permission_request(n));
    }

    // Permissions asked for (names as in llscriptruntimeperms.h), and whether any is one the
    // viewer flags with a caution (taking L$, privileged land access).
    LLSD requested_permissions(const LLNotificationPtr& n, bool& caution)
    {
        caution = false;
        LLSD names = LLSD::emptyArray();
        const U32 questions = (U32)n->getPayload()["questions"].asInteger();
        for (const script_perm_t& perm : SCRIPT_PERMISSIONS)
        {
            if (questions & perm.permbit)
            {
                names.append(perm.question);
                caution |= perm.caution;
            }
        }
        return names;
    }

    void ensure_script_dialogs_hooked()
    {
        if (sScriptDialogConnection.connected())
            return;
        LLNotificationChannelPtr channel = LLNotifications::instance().getChannel("Visible");
        if (!channel)
            return;
        sScriptDialogConnection = channel->connectChanged([](const LLSD& payload)
        {
            const std::string sig = payload["sigtype"].asString();
            const LLUUID id = payload["id"].asUUID();
            if (sig == "add" || sig == "load")
            {
                if (is_script_dialog(LLNotifications::instance().find(id)))
                    sScriptDialogs.insert(id);
            }
            else if (sig == "delete" || sig == "cancel")
            {
                sScriptDialogs.erase(id);
            }
            return false;
        });
    }

    // Grab now, release after `hold` seconds: what a left click on a touchable object sends.
    LLSD send_touch(LLViewerObject* obj, const LLPickInfo& pick, F32 hold)
    {
        send_ObjectGrab_message(obj, pick, LLVector3::zero);
        LLPointer<LLViewerObject> keep(obj);
        LLPickInfo release(pick);
        doAfterInterval([keep, release]()
        {
            if (!keep->isDead())
                send_ObjectDeGrab_message(keep, release);
        }, llclamp(hold, 0.05f, 10.f));
        LLSD touched = llsd::map(
            "id", obj->getID(),
            "root_id", obj->getRootEdit()->getID(),
            "face", pick.mObjectFace,
            "st", llsd::array(pick.mSTCoords.mV[VX], pick.mSTCoords.mV[VY]),
            "is_hud", obj->isHUDAttachment());
        if (auto found = sObjectProps.find(obj->getID()); found != sObjectProps.end())
            touched["name"] = found->second["name"];
        return touched;
    }
}

void LLBuildListener::getHudObjects(LLSD const& data)
{
    Response response(LLSD(), data);
    if (!isAgentAvatarValid())
    {
        return response.error("Avatar not ready");
    }
    LLSD huds = LLSD::emptyArray();
    for (const auto& [index, attachment] : gAgentAvatarp->mAttachmentPoints)
    {
        if (!attachment || !attachment->getIsHUDAttachment())
            continue;
        for (const LLPointer<LLViewerObject>& root : attachment->mAttachedObjects)
        {
            if (!root || root->isDead())
                continue;
            LLSD prims = LLSD::emptyArray();
            auto add_prim = [&prims](LLViewerObject* prim)
            {
                LLSD entry = llsd::map(
                    "id", prim->getID(),
                    "local_pos", vec3_sd(prim->getPosition()),
                    "scale", vec3_sd(prim->getScale()),
                    "num_faces", LLSD::Integer(prim->getNumTEs()),
                    "is_root", prim->isRootEdit());
                if (auto found = sObjectProps.find(prim->getID()); found != sObjectProps.end())
                {
                    entry["name"] = found->second["name"];
                    entry["desc"] = found->second["desc"];
                }
                prims.append(entry);
            };
            add_prim(root);
            for (LLViewerObject* child : root->getChildren())
            {
                if (child && !child->isAvatar())
                    add_prim(child);
            }
            LLSD hud = llsd::map("attach_point", attachment->getName(),
                                 "root_id", root->getID(),
                                 "item_id", root->getAttachmentItemID(),
                                 "prims", prims);
            if (auto found = sObjectProps.find(root->getID()); found != sObjectProps.end())
                hud["name"] = found->second["name"];
            huds.append(hud);
        }
    }
    response["huds"] = huds;
}

void LLBuildListener::getScreenInfo(LLSD const& data)
{
    Response response(LLSD(), data);
    response["width"] = gViewerWindow->getWindowWidthRaw();
    response["height"] = gViewerWindow->getWindowHeightRaw();
    response["ui_scale"] = gViewerWindow->getDisplayScale().mV[VX];
}

void LLBuildListener::touchObject(LLSD const& data)
{
    Response response(LLSD(), data);
    LLViewerObject* obj = find_editable(response, data["id"], false);
    if (!obj) return;
    LLPickInfo pick;
    pick.mObjectID = obj->getID();
    pick.mObjectFace = data.has("face") ? data["face"].asInteger() : -1;
    if (data.has("st"))
    {
        pick.mSTCoords.set((F32)data["st"][0].asReal(), (F32)data["st"][1].asReal());
    }
    else
    {
        pick.mSTCoords.set(-1.f, -1.f); // TOUCH_INVALID_TEXCOORD
    }
    pick.mUVCoords = pick.mSTCoords;
    pick.mIntersection = obj->getPositionAgent();
    response["touched"] = send_touch(obj, pick, data.has("hold") ? (F32)data["hold"].asReal() : 0.1f);
}

void LLBuildListener::touchAt(LLSD const& data)
{
    Response response(LLSD(), data);
    // Raw pixels, top-left origin -> scaled GL coordinates (bottom-left origin) used for picking.
    const F32 scale = gViewerWindow->getDisplayScale().mV[VX];
    const F32 raw_h = (F32)gViewerWindow->getWindowHeightRaw();
    S32 x = ll_round((F32)data["x"].asReal() / scale);
    S32 y = ll_round((raw_h - (F32)data["y"].asReal()) / scale);
    LLPickInfo pick = gViewerWindow->pickImmediate(x, y, false /* transparent */, false /* rigged */,
                                                   false /* particles */, true /* unselectable */);
    LLViewerObject* obj = pick.getObject();
    if (!obj || obj->isAvatar())
    {
        return response.error(obj ? "That point is an avatar, not an object" : "Nothing touchable at that point");
    }
    // A real left click follows the prim's click action (LLToolPie): honour "sit" so chairs work.
    // Buy/pay/open are deliberately not followed; the object just gets touched.
    U8 action = obj->getClickAction();
    LLViewerObject* parent = obj->getRootEdit();
    if (action == CLICK_ACTION_NONE && parent && parent != obj)
    {
        action = parent->getClickAction();
    }
    if (action == CLICK_ACTION_SIT && isAgentAvatarValid() && !gAgentAvatarp->isSitting()
        && !obj->isHUDAttachment())
    {
        handle_object_sit(obj, pick.mObjectOffset);
        response["touched"] = llsd::map("id", obj->getID(), "root_id", obj->getRootEdit()->getID(),
                                        "action", "sit", "is_hud", false);
        return;
    }
    response["touched"] = send_touch(obj, pick, data.has("hold") ? (F32)data["hold"].asReal() : 0.1f);
}

void LLBuildListener::getScriptDialogs(LLSD const& data)
{
    Response response(LLSD(), data);
    ensure_script_dialogs_hooked();
    LLSD dialogs = LLSD::emptyArray();
    for (auto it = sScriptDialogs.begin(); it != sScriptDialogs.end();)
    {
        LLNotificationPtr n = LLNotifications::instance().find(*it);
        if (!n || n->isCancelled() || n->isRespondedTo())
        {
            it = sScriptDialogs.erase(it);
            continue;
        }
        LLSD buttons = LLSD::emptyArray();
        bool textbox = false;
        LLNotificationFormPtr form = n->getForm();
        for (S32 i = 0; form && i < form->getNumElements(); ++i)
        {
            LLSD element = form->getElement(i);
            if (element["type"].asString() != "button")
                continue;
            if (element["name"].asString() == TEXTBOX_MAGIC_TOKEN)
                textbox = true;
            else
                buttons.append(element["name"]);
        }
        const LLSD& payload = n->getPayload();
        const LLSD& subs = n->getSubstitutions();
        if (is_permission_request(n))
        {
            bool caution = false;
            LLSD perms = requested_permissions(n, caution);
            dialogs.append(llsd::map(
                "id", n->getID(),
                "kind", "permission",
                "object_name", subs["OBJECTNAME"],
                "owner_name", subs["NAME"],
                "object_id", payload["task_id"],
                "permissions", perms,
                "needs_user", caution,
                "buttons", buttons));
            ++it;
            continue;
        }
        dialogs.append(llsd::map(
            "kind", "menu",
            "id", n->getID(),
            "object_name", payload["object_name"],
            "object_id", payload["object_id"],
            "channel", payload["chat_channel"],
            "message", subs["MESSAGE"],
            "text_box", textbox,
            "buttons", buttons));
        ++it;
    }
    response["dialogs"] = dialogs;
}

void LLBuildListener::answerScriptDialog(LLSD const& data)
{
    Response response(LLSD(), data);
    LLNotificationPtr n = LLNotifications::instance().find(data["id"].asUUID());
    if (!is_script_dialog(n) || n->isCancelled() || n->isRespondedTo())
    {
        return response.error("No such open script dialog (it may have timed out)");
    }
    if (!data["ignore"].asBoolean())
    {
        LLSD answer = n->getResponseTemplate();
        if (data.has("text"))
        {
            const std::string text = data["text"].asString();
            if (text.empty())
                answer[TEXTBOX_MAGIC_TOKEN] = true; // submitted blank text, not ignored
            else
                answer[TEXTBOX_MAGIC_TOKEN] = text;
        }
        else
        {
            const std::string button = data["button"].asString();
            if (!answer.has(button))
            {
                return response.error(stringize("The dialog has no button ", std::quoted(button)));
            }
            if (is_permission_request(n))
            {
                bool caution = false;
                requested_permissions(n, caution);
                LLNotificationFormPtr form = n->getForm();
                bool grants = form && form->getNumElements() > 0
                              && LLNotification::getSelectedOption(n->asLLSD(), LLSD().with(button, true)) == 0;
                if (grants && caution)
                {
                    // Taking the user's money (or privileged land access) is never granted by a tool.
                    return response.error("This request includes taking L$ or privileged access: only the user "
                                          "may grant it, in the viewer");
                }
            }
            answer[button] = true;
        }
        n->respond(answer);
    }
    LLNotifications::instance().cancel(n); // close the blue menu / text box
    sScriptDialogs.erase(n->getID());
}
