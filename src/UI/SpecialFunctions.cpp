#include "SpecialFunctions.h"
#include "OpenFunscripter.h"
#include "imgui.h"
#include "imgui_stdlib.h"

#include "OFS_Lua.h"

#include <filesystem>
#include <sstream>

#include "SDL_thread.h"

SpecialFunctionsWindow::SpecialFunctionsWindow() noexcept
{
    SetFunction((SpecialFunctions)OpenFunscripter::ptr->settings->data().currentSpecialFunction);
}

void SpecialFunctionsWindow::SetFunction(SpecialFunctions functionEnum) noexcept
{
	switch (functionEnum) {
	case SpecialFunctions::RANGE_EXTENDER:
		function = std::make_unique<FunctionRangeExtender>();
		break;
    case SpecialFunctions::RAMER_DOUGLAS_PEUCKER:
        function = std::make_unique<RamerDouglasPeucker>();
        break;
    case SpecialFunctions::CUSTOM_LUA_FUNCTIONS:
        function = std::make_unique<CustomLua>();
        break;
	default:
		break;
	}
}

void SpecialFunctionsWindow::ShowFunctionsWindow(bool* open) noexcept
{
	if (open != nullptr && !(*open)) { return; }
	ImGui::Begin(SpecialFunctionsId, open, ImGuiWindowFlags_None);
	ImGui::SetNextItemWidth(-1.f);
    if (ImGui::Combo("##Functions", (int32_t*)&OpenFunscripter::ptr->settings->data().currentSpecialFunction,
        "Range extender\0"
        "Simplify (Ramer-Douglas-Peucker)\0"
        "Custom functions\0"
        "\0")) {
        SetFunction((SpecialFunctions)OpenFunscripter::ptr->settings->data().currentSpecialFunction);
    }
	ImGui::Spacing();
	function->DrawUI();
    Util::ForceMinumumWindowSize(ImGui::GetCurrentWindow());
	ImGui::End();
}

inline Funscript& FunctionBase::ctx() noexcept
{
	return OpenFunscripter::script();
}

// range extender
FunctionRangeExtender::FunctionRangeExtender() noexcept
{
    auto app = OpenFunscripter::ptr;
    app->events->Subscribe(EventSystem::FunscriptSelectionChangedEvent, EVENT_SYSTEM_BIND(this, &FunctionRangeExtender::SelectionChanged));
}

FunctionRangeExtender::~FunctionRangeExtender() noexcept
{
    auto app = OpenFunscripter::ptr;
    app->events->Unsubscribe(EventSystem::FunscriptSelectionChangedEvent, this);
}

void FunctionRangeExtender::SelectionChanged(SDL_Event& ev) noexcept
{
    if (OpenFunscripter::script().SelectionSize() > 0) {
        rangeExtend = 0;
        createUndoState = true;
    }
}

void FunctionRangeExtender::DrawUI() noexcept
{
    auto app = OpenFunscripter::ptr;
    auto undoSystem = app->script().undoSystem.get();
    if (app->script().SelectionSize() > 4 || (undoSystem->MatchUndoTop(StateType::RANGE_EXTEND))) {
        if (ImGui::SliderInt("Range", &rangeExtend, -50, 100)) {
            rangeExtend = Util::Clamp<int32_t>(rangeExtend, -50, 100);
            if (createUndoState || 
                !undoSystem->MatchUndoTop(StateType::RANGE_EXTEND)) {
                undoSystem->Snapshot(StateType::RANGE_EXTEND);
            }
            else {
                undoSystem->Undo();
                undoSystem->Snapshot(StateType::RANGE_EXTEND);
            }
            createUndoState = false;
            ctx().RangeExtendSelection(rangeExtend);
        }
    }
    else
    {
        ImGui::Text("Select atleast 5 actions to extend.");
    }
}

RamerDouglasPeucker::RamerDouglasPeucker() noexcept
{
    auto app = OpenFunscripter::ptr;
    app->events->Subscribe(EventSystem::FunscriptSelectionChangedEvent, EVENT_SYSTEM_BIND(this, &RamerDouglasPeucker::SelectionChanged));
}

RamerDouglasPeucker::~RamerDouglasPeucker() noexcept
{
    OpenFunscripter::ptr->events->UnsubscribeAll(this);
}

void RamerDouglasPeucker::SelectionChanged(SDL_Event& ev) noexcept
{
    if (OpenFunscripter::script().SelectionSize() > 0) {
        epsilon = 0.f;
        createUndoState = true;
    }
}

inline static double PerpendicularDistance(const FunscriptAction pt, const FunscriptAction lineStart, const FunscriptAction lineEnd) noexcept
{
    double dx = (double)lineEnd.at - lineStart.at;
    double dy = (double)lineEnd.pos - lineStart.pos;

    //Normalise
    double mag = std::sqrt(dx * dx + dy * dy);
    if (mag > 0.0)
    {
        dx /= mag; dy /= mag;
    }

    double pvx = (double)pt.at - lineStart.at;
    double pvy = (double)pt.pos - lineStart.pos;

    //Get dot product (project pv onto normalized direction)
    double pvdot = dx * pvx + dy * pvy;

    //Scale line direction vector
    double dsx = pvdot * dx;
    double dsy = pvdot * dy;

    //Subtract this from pv
    double ax = pvx - dsx;
    double ay = pvy - dsy;

    return std::sqrt(ax * ax + ay * ay);
}

inline static void RamerDouglasPeuckerAlgo(const std::vector<FunscriptAction>& pointList, double epsilon, std::vector<FunscriptAction>& out) noexcept
{
    // Find the point with the maximum distance from line between start and end
    double dmax = 0.0;
    size_t index = 0;
    size_t end = pointList.size() - 1;
    for (size_t i = 1; i < end; i++)
    {
        double d = PerpendicularDistance(pointList[i], pointList[0], pointList[end]);
        if (d > dmax)
        {
            index = i;
            dmax = d;
        }
    }

    // If max distance is greater than epsilon, recursively simplify
    if (dmax > epsilon)
    {
        // Recursive call
        std::vector<FunscriptAction> recResults1;
        std::vector<FunscriptAction> recResults2;
        std::vector<FunscriptAction> firstLine(pointList.begin(), pointList.begin() + index + 1);
        std::vector<FunscriptAction> lastLine(pointList.begin() + index, pointList.end());
        RamerDouglasPeuckerAlgo(firstLine, epsilon, recResults1);
        RamerDouglasPeuckerAlgo(lastLine, epsilon, recResults2);

        // Build the result list
        out.assign(recResults1.begin(), recResults1.end() - 1);
        out.insert(out.end(), recResults2.begin(), recResults2.end());
    }
    else
    {
        //Just return start and end points
        out.clear();
        out.push_back(pointList[0]);
        out.push_back(pointList[end]);
    }
}

void RamerDouglasPeucker::DrawUI() noexcept
{
    auto app = OpenFunscripter::ptr;
    auto undoSystem = app->script().undoSystem.get();
    if (app->script().SelectionSize() > 4 || (undoSystem->MatchUndoTop(StateType::SIMPLIFY))) {
        if (ImGui::DragFloat("Epsilon", &epsilon, 0.1f)) {
            epsilon = std::max(epsilon, 0.f);
            if (createUndoState ||
                !undoSystem->MatchUndoTop(StateType::SIMPLIFY)) {
                undoSystem->Snapshot(StateType::SIMPLIFY);
            }
            else {
                undoSystem->Undo();
                undoSystem->Snapshot(StateType::SIMPLIFY);
            }
            createUndoState = false;
            auto selection = ctx().Selection();
            ctx().RemoveSelectedActions();
            std::vector<FunscriptAction> newActions;
            newActions.reserve(selection.size());
            RamerDouglasPeuckerAlgo(selection, epsilon, newActions);
            for (auto&& action : newActions) {
                ctx().AddAction(action);
            }
        }
    }
    else
    {
        ImGui::Text("Select atleast 5 actions to simplify.");
    }
}


CustomLua::CustomLua() noexcept
{
    auto app = OpenFunscripter::ptr;
    app->events->Subscribe(EventSystem::FunscriptSelectionChangedEvent, EVENT_SYSTEM_BIND(this, &CustomLua::SelectionChanged));
    resetVM();
    if (Thread.L != nullptr) {
        updateScripts();
    }
    else {
        LOG_ERROR("Failed to create lua vm.");
    }
}

CustomLua::~CustomLua() noexcept
{
    if (Thread.L != nullptr) {
        lua_close(Thread.L);
    }
    OpenFunscripter::ptr->events->UnsubscribeAll(this);
}

void CustomLua::SelectionChanged(SDL_Event& ev) noexcept
{
    if (OpenFunscripter::script().SelectionSize() > 0) {
        createUndoState = true;
    }
}

void CustomLua::updateScripts() noexcept
{
    auto luaPathString = Util::Resource("lua");
    auto luaPath = Util::PathFromString(luaPathString);
    Util::CreateDirectories(luaPathString);

    scripts.clear();

    std::error_code ec;
    auto iterator = std::filesystem::directory_iterator(luaPath, ec);
    for (auto it = std::filesystem::begin(iterator); it != std::filesystem::end(iterator); it++) {
        auto filename = it->path().filename().u8string();
        auto name = it->path().filename();
        name.replace_extension("");

        auto extension = it->path().extension().u8string();
        if (!filename.empty() && extension == ".lua") {
#ifdef NDEBUG
            if (name == "funscript") { continue; }
#endif
            scripts.emplace_back(std::move(filename));
        }
    }
}

void CustomLua::resetVM() noexcept
{
    auto& L = Thread.L;
    if (L != nullptr) {
        lua_close(L);
        L = nullptr;
    }
    L = luaL_newstate();
    if (L != nullptr) {
        luaL_openlibs(L);

        auto initScript = Util::Resource("lua/funscript.lua");
        int result = luaL_dofile(L, initScript.c_str());
        if (result > 0) {
            LOGF_ERROR("lua init script error: %s", lua_tostring(L, -1));
        }

        auto app = OpenFunscripter::ptr;
        auto& script = app->ActiveFunscript();
        auto& actions = script->Actions();

        // HACK: this has awful performance like 15000 actions block for 5+ seconds...
        std::stringstream builder;
        char tmp[1024];
        for (auto&& action : actions) {
            stbsp_snprintf(tmp, sizeof(tmp), "CurrentScript:AddAction(%d, %d, %s)\n",
                action.at,
                action.pos,
                script->IsSelected(action) ? "true" : "false"
            );
            builder << tmp;
        }
        for (auto&& action : app->FunscriptClipboard()) {
            stbsp_snprintf(tmp, sizeof(tmp), "Clipboard:AddAction(%d, %d, false)\n",
                action.at,
                action.pos
            );
            builder << tmp;
        }
        stbsp_snprintf(tmp, sizeof(tmp), "CurrentTimeMs=%lf\n", app->player.getCurrentPositionMsInterp());
        builder << tmp;
        stbsp_snprintf(tmp, sizeof(tmp), "FrameTimeMs=%lf\n", app->player.getFrameTimeMs());
        builder << tmp;
            
        Thread.setupScript = builder.str();
    }
}

void CustomLua::runScript(const std::string& path) noexcept
{
    if (Thread.running) return;
    Thread.running = true; 

    resetVM();
    auto luaThread = [](void* user) -> int {
        LuaThread& data = *(LuaThread*)user;

        LOG_INFO("============= SETUP =============");
        LOG_INFO("Loading script into lua...");
        auto startTime = std::chrono::high_resolution_clock::now();
        luaL_dostring(data.L, data.setupScript.c_str());
        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
        LOGF_INFO("setup time: %ld ms", duration.count());

        LOGF_INFO("path: %s", data.path.c_str());
        LOG_INFO("============= RUN LUA =============");
        startTime = std::chrono::high_resolution_clock::now();
        data.result = luaL_dofile(data.L, data.path.c_str());
        LOGF_INFO("lua result: %d", data.result);
        if (data.result > 0) {
            LOGF_ERROR("lua error: %s", lua_tostring(data.L, -1));
            data.running = false;
        }
        else {
            endTime = std::chrono::high_resolution_clock::now();
            duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
            LOGF_INFO("execution time: %ld ms", duration.count());

            data.collected.clear();
            data.selection.clear();
            if (CollectScript(data, data.L)) {
                // fire event to the main thread
                auto eventData = new EventSystem::SingleShotEventData;
                eventData->ctx = &data;
                
                eventData->handler = [](void* ctx) {
                    // script finished handler
                    // this code executes on the main thread during event processing
                    LuaThread& data = *(LuaThread*)ctx;

                    auto app = OpenFunscripter::ptr;
                    if (data.currentScriptIdx < app->LoadedFunscripts.size()) {
                        auto& script = app->LoadedFunscripts[data.currentScriptIdx];
                        script->undoSystem->Snapshot(StateType::CUSTOM_LUA);
                        // TODO: HACK
                        script->SelectAll();
                        script->RemoveSelectedActions();
                        for (auto&& act : data.collected) {
                            script->AddActionSafe(act);
                        }
                        for (auto&& act : data.selection) {
                            script->SetSelection(act, true);
                        }
                    }
                    app->player.setPosition(data.NewPositionMs);

                    data.collected.clear();
                    data.collected.shrink_to_fit();
                    data.selection.clear();
                    data.selection.shrink_to_fit();
                    data.running = false;
                };

                SDL_Event ev;
                ev.type = EventSystem::SingleShotEvent;
                ev.user.data1 = eventData;
                SDL_PushEvent(&ev);
            }
        }
        LOG_INFO("================ END ===============\n\n");
        return 0;
    };
    Thread.currentScriptIdx = OpenFunscripter::ptr->ActiveFunscriptIndex();
    Thread.path = path;
    auto thread = SDL_CreateThread(luaThread, "CustomLuaScript", &Thread);
    SDL_DetachThread(thread);
}

bool CustomLua::CollectScript(LuaThread& thread, lua_State* L) noexcept
{
    lua_getglobal(L, "CurrentScript");
    // global
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "actions");
        // CurrentScript.actions
        if (lua_istable(L, -1)) {
            int32_t size = lua_rawlen(L, -1);
            for (int i = 1; i <= size; i++) {
                lua_rawgeti(L, -1, i); // push action
                if (lua_istable(L, -1)) {
                    int32_t at;
                    int32_t pos;
                    bool selected;

                    lua_getfield(L, -1, "at");
                    if (lua_isnumber(L, -1)) {
                        at = lua_tonumber(L, -1);
                    }
                    else {
                        LOG_ERROR("Abort. Couldn't read \"at\" timestamp property.");
                        return false;
                    }
                    lua_pop(L, 1); // pop at

                    lua_getfield(L, -1, "pos");
                    if (lua_isnumber(L, -1)) {
                        pos = lua_tonumber(L, -1);
                    }
                    else {
                        LOG_ERROR("Abort. Couldn't read position property.");
                        return false;
                    }
                    lua_pop(L, 1); // pop pos

                    lua_getfield(L, -1, "selected");
                    if (lua_isboolean(L, -1)) {
                        selected = lua_toboolean(L, -1);
                    }
                    else {
                        LOG_ERROR("Abort. Couldn't read selected property.");
                        return false;
                    }
                    lua_pop(L, 1); // pop selected

                    thread.collected.push_back(FunscriptAction(at, pos));
                    if (selected) {
                        thread.selection.push_back(FunscriptAction(at, pos));
                    }
                }
                lua_pop(L, 1); // pop action
            }
            lua_pop(L, 2); // pop CurrentScript.actions & CurrentScript
            lua_getglobal(L, "CurrentTimeMs");
            if (lua_isnumber(L, -1)) {
                thread.NewPositionMs = lua_tonumber(L, -1);
            }
            else {
                LOG_ERROR("Abort. Couldn't read CurrentTimeMs property.");
                return false;
            }
            return true;
        }
    }
    return false;
}

void CustomLua::DrawUI() noexcept
{
    if (ImGui::Button("Reload scripts")) { updateScripts(); }
    ImGui::SameLine();
    if (ImGui::Button("Script directory")) { Util::OpenFileExplorer(Util::Resource("lua").c_str()); }
    ImGui::Spacing(); ImGui::SeparatorEx(ImGuiSeparatorFlags_Horizontal); ImGui::Spacing();
    if (Thread.running) {
        ImGui::TextUnformatted("Running script...");
    }
    else {
        for (auto&& script : scripts) {
            if (ImGui::Button(script.c_str())) {
                auto pathString = Util::Resource("lua");
                auto scriptPath = Util::PathFromString(pathString);
                Util::ConcatPathSafe(scriptPath, script);
                runScript(scriptPath.u8string());
            }
        }
    }
    ImGui::Spacing(); ImGui::SeparatorEx(ImGuiSeparatorFlags_Horizontal); ImGui::Spacing();
}
