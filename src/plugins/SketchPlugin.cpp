#include "ui/LengthField.h"
#include "../ui/UiTheme.h"
#include "ui_scale.h"
#include "../touch_mode.h"
#include "../plugin/PluginMacro.h"
#include "../plugin/PluginContext.h"
#include "../plugin/InteractiveTool.h"
#include "../plugin/PluginRegistry.h"
#include "../core/Document.h"
#include "../core/History.h"
#include "../core/SelectionManager.h"
#include "../core/Events.h"
#include "../core/EventBus.h"
#include "../modeling/Sketch.h"
#include "../core/NumParse.h"
#include "../modeling/SketchTool.h"
#include "../modeling/SketchSolver.h"
#include "../modeling/SketchEditOp.h"
#include "../modeling/ExtrudeOp.h"
#include "../viewport/Camera.h"
#include <TopoDS.hxx>
#include <BRep_Tool.hxx>
#include <Geom_Plane.hxx>
#include <Geom_Surface.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <imgui.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <functional>
#include "../i18n.h"

namespace {

using namespace materializr;

class SketchModeTool : public InteractiveTool {
public:
    explicit SketchModeTool(int editSketchId = -1, TopoDS_Face faceForNew = {})
        : m_editSketchId(editSketchId), m_faceForNew(faceForNew) {}

    void begin(PluginContext& ctx) override {
        m_sketchTool = std::make_unique<SketchTool>();
        m_solver = std::make_unique<SketchSolver>();

        if (m_editSketchId >= 0) {
            m_sketch = ctx.document().getSketch(m_editSketchId);
            if (!m_sketch) { m_done = true; return; }
        } else {
            m_sketch = std::make_shared<Sketch>();
            if (!m_faceForNew.IsNull()) {
                Handle(Geom_Surface) surf = BRep_Tool::Surface(m_faceForNew);
                if (!surf.IsNull() && surf->IsKind(STANDARD_TYPE(Geom_Plane))) {
                    Handle(Geom_Plane) geomPlane = Handle(Geom_Plane)::DownCast(surf);
                    m_sketch->setPlane(geomPlane->Pln());
                    m_sketch->setSourceFace(m_faceForNew);
                }
            }
        }

        m_sketchTool->setSketch(m_sketch.get());
        m_sketchTool->setSolver(m_solver.get());
        m_sketchTool->setMode(SketchToolMode::Line);
        m_gridStep = 1.0f;

        alignCamera(ctx);
        ctx.events().publish(SketchModeEnteredEvent{m_editSketchId});
    }

    bool update(PluginContext&) override { return !m_done; }

    void commit(PluginContext& ctx) override {
        m_sketchTool->onConfirm();
        exitSketch(ctx);
    }

    void cancel(PluginContext& ctx) override {
        m_sketchTool->onCancel();
        exitSketch(ctx);
    }

    bool handleInput(PluginContext& ctx, const ToolInputEvent& event) override {
        if (event.type == ToolInputEvent::KeyPress) {
            if (event.key == 526) { cancel(ctx); return true; } // Escape
            if (event.key == 525) { // Enter - finish sketch
                recordMutation(ctx, [&]{ m_sketchTool->onConfirm(); });
                commit(ctx);
                return true;
            }
        }
        return false;
    }

    void renderOverlay(PluginContext& ctx) override {
        if (m_sketchTool->getMode() == SketchToolMode::Dimension) {
            const char* msg = "DIMENSION - Click an entity to dimension.";
            switch (m_sketchTool->getDimPhase()) {
                case DimPhase::PickSecondOrPlace:
                    msg = "DIMENSION - Click a second entity, or empty space to place.";
                    break;
                case DimPhase::PlaceLabel:
                    msg = "DIMENSION - Click to place the label.";
                    break;
                default: break;
            }
            materializr::viewportBanner(materializr::accentText(), "%s", msg);
        } else {
            materializr::viewportBanner(
                materializr::accentText(),
                materializr::touchMode()
                    ? "SKETCH MODE - Draw shapes. Finish Sketch applies, Exit Sketch discards."
                    : "SKETCH MODE - Draw shapes. Enter to finish, Escape to cancel.");
        }

        // Dimension input when placing
        if (m_sketchTool->hasPreview()) {
            ImGui::SetNextWindowPos(ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - 200,
                                            ImGui::GetWindowPos().y + ImGui::GetWindowHeight() - 60));
            ImGui::SetNextWindowSize(uiSz(180, 0));
            ImGui::Begin("##SketchDim", nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
            if (ImGui::InputText(materializr::unitSuffix(), m_dimBuf, sizeof(m_dimBuf),
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
                float v = 0.0f;
                // Only a LENGTH is converted. An arc's sweep is degrees and a
                // polygon's first value is a side count; running those through
                // parseLength made a typed 6 sides arrive as 152 under inches.
                const bool dimIsLen = m_sketchTool->dimensionValueIsLength();
                const bool dimOk = dimIsLen ? materializr::parseLength(m_dimBuf, v)
                                            : materializr::parseFinite(m_dimBuf, v);
                if (dimOk && v > 0.0f) {
                    recordMutation(ctx, [&]{ m_sketchTool->applyDimension(v); });
                }
                m_dimBuf[0] = '\0';
            }
            ImGui::End();
        }
    }

    std::string name() const override { return "Sketch Mode"; }

    // Accessors for Application's sketch rendering integration
    Sketch* getActiveSketch() const { return m_sketch.get(); }
    SketchTool* getSketchTool() const { return m_sketchTool.get(); }
    SketchSolver* getSolver() const { return m_solver.get(); }
    float getGridStep() const { return m_gridStep; }
    int getSketchId() const { return m_editSketchId; }

    void setMode(SketchToolMode mode) {
        if (m_sketchTool) m_sketchTool->setMode(mode);
    }

    void handleMouseDown(PluginContext& ctx, glm::vec2 sketchCoord) {
        recordMutation(ctx, [&]{ m_sketchTool->onMouseDown(sketchCoord); });
    }

    void handleMouseUp(glm::vec2 sketchCoord) {
        m_sketchTool->onMouseUp(sketchCoord);
    }

    void handleMouseMove(glm::vec2 sketchCoord) {
        m_sketchTool->onMouseMove(sketchCoord);
    }

private:
    void alignCamera(PluginContext& ctx) {
        if (!m_sketch) return;
        const gp_Pln& pln = m_sketch->getPlane();
        const gp_Ax3& ax = pln.Position();
        gp_Pnt o = ax.Location();
        gp_Dir n = ax.Direction();
        gp_Dir y = ax.YDirection();

        glm::vec3 planeOrigin(o.X(), o.Y(), o.Z());
        glm::vec3 normal(n.X(), n.Y(), n.Z());
        glm::vec3 up(y.X(), y.Y(), y.Z());

        float orthoSize = std::max(20.0f, m_gridStep * 40.0f);
        if (!m_sketch->getSourceFace().IsNull()) {
            try {
                Bnd_Box bb;
                BRepBndLib::Add(m_sketch->getSourceFace(), bb);
                if (!bb.IsVoid()) {
                    double xmin, ymin, zmin, xmax, ymax, zmax;
                    bb.Get(xmin, ymin, zmin, xmax, ymax, zmax);
                    float dx = static_cast<float>(xmax - xmin);
                    float dy = static_cast<float>(ymax - ymin);
                    float dz = static_cast<float>(zmax - zmin);
                    float diag = 0.5f * std::sqrt(dx*dx + dy*dy + dz*dz);
                    if (diag > 1e-3f) orthoSize = diag * 1.2f;
                }
            } catch (...) {}
        }

        Camera& cam = const_cast<Camera&>(ctx.camera());
        float standoff = std::max(orthoSize * 4.0f, 10.0f);
        cam.setTarget(planeOrigin);
        cam.setPosition(planeOrigin + normal * standoff);
        cam.setUp(up);
        cam.setOrthoSize(orthoSize);
        cam.setOrthographic(true);
    }

    void exitSketch(PluginContext& ctx) {
        m_sketchTool->setMode(SketchToolMode::None);
        m_sketchTool->setSketch(nullptr);
        m_sketchTool->setSolver(nullptr);

        if (m_sketch && m_sketch->elementCount() > 0) {
            if (m_editSketchId < 0) {
                m_editSketchId = ctx.document().addSketch(m_sketch);
            }
        }

        ctx.events().publish(SketchModeExitedEvent{});
        ctx.markMeshesDirty();
        m_done = true;
    }

    void recordMutation(PluginContext& ctx, const std::function<void()>& mutator) {
        if (!m_sketch) { mutator(); return; }
        auto signature = [](const Sketch& s) {
            size_t h = 1469598103934665603ull;
            auto mix = [&](size_t v) { h = (h ^ v) * 1099511628211ull; };
            mix(s.getLines().size());
            for (const auto& l : s.getLines()) mix(static_cast<size_t>(l.id));
            mix(s.getCircles().size());
            for (const auto& c : s.getCircles()) mix(static_cast<size_t>(c.id));
            mix(s.getArcs().size());
            for (const auto& a : s.getArcs()) mix(static_cast<size_t>(a.id));
            mix(s.getSplines().size());
            for (const auto& sp : s.getSplines()) mix(static_cast<size_t>(sp.id));
            mix(s.getPolygons().size());
            for (const auto& p : s.getPolygons()) mix(static_cast<size_t>(p.id));
            return h;
        };
        size_t beforeSig = signature(*m_sketch);
        auto before = std::make_shared<Sketch>(*m_sketch);
        mutator();
        size_t afterSig = signature(*m_sketch);
        if (afterSig == beforeSig) return;
        auto after = std::make_shared<Sketch>(*m_sketch);
        auto op = std::make_unique<SketchEditOp>(m_sketch, std::move(before), std::move(after));
        ctx.history().pushExecuted(std::move(op));
    }

    std::shared_ptr<Sketch> m_sketch;
    std::unique_ptr<SketchTool> m_sketchTool;
    std::unique_ptr<SketchSolver> m_solver;
    float m_gridStep = 1.0f;
    int m_editSketchId = -1;
    TopoDS_Face m_faceForNew;
    char m_dimBuf[32] = "";
    bool m_done = false;
};

} // anonymous namespace

REGISTER_PLUGIN(Sketch, [](PluginContext& /*ctx*/) {
    // Sketch entry and tool switching are handled entirely by the Application's
    // own sketch path via the Toolbar's ToolActions (Sketch on XY/XZ/YZ, Sketch
    // on Face, and the in-sketch Line/Circle/… buttons).
    //
    // The plugin SketchModeTool below is intentionally NOT wired to any command
    // or button: Application never calls InteractiveTool::handleInput(), so the
    // tool can't receive viewport input, and once activated its renderOverlay()
    // runs outside any ImGui window — which spawned a stray "Debug" window. The
    // class is kept only as a reference for a future input-routing migration.
})
