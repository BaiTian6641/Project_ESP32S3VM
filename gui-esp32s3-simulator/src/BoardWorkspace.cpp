#include "BoardWorkspace.h"
#include "PeripheralManager.h"
#include "ThemeManager.h"
#include "ProjectDocument.h"
#include <QAction>
#include <QGraphicsSceneMouseEvent>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QTemporaryDir>
#include <QToolButton>
#include <QUuid>
#include <algorithm>
#include <QFormLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QScrollArea>
#include <QTabWidget>
#include <QtMath>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGraphicsView>
#include <QGraphicsRectItem>
#include <QGraphicsSimpleTextItem>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QPainterPath>
#include <QPushButton>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QResizeEvent>
#include <QWheelEvent>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QPainter>
#include <QStyleOptionGraphicsItem>
#include <QScrollBar>
#include <QTimer>
#include <functional>

namespace {
constexpr int BrushRole = Qt::UserRole + 1;
constexpr int PenRole = Qt::UserRole + 2;
constexpr int ComponentRole = Qt::UserRole + 3;
constexpr int TerminalRole = Qt::UserRole + 4;
constexpr int NetRole = Qt::UserRole + 5;
enum ColorRole { Background = 1, Layer, PrimaryText, SecondaryText, HelperText,
    StrongBorder, Grid, I2c, Spi, Uart, Gpio };
bool validGpio(int gpio) { return gpio >= 0 && gpio <= 48 && (gpio < 22 || gpio > 25); }
ColorRole busColorRole(const QString &kind) {
    if (kind == "i2c") return I2c;
    if (kind == "spi") return Spi;
    if (kind == "uart") return Uart;
    return Gpio;
}
QColor colorFor(int role) {
    const auto &t = ThemeManager::instance()->tokens();
    switch (role) {
    case Background: return t.background;
    case Layer: return t.layer;
    case PrimaryText: return t.textPrimary;
    case SecondaryText: return t.textSecondary;
    case HelperText: return t.textHelper;
    case StrongBorder: return t.borderStrong;
    case Grid: return t.canvasGrid;
    case I2c: return t.busI2c;
    case Spi: return t.busSpi;
    case Uart: return t.busUart;
    default: return t.busGpio;
    }
}
void styleItem(QAbstractGraphicsShapeItem *item, int brush, int pen = 0) {
    item->setData(BrushRole, brush);
    item->setData(PenRole, pen);
    if (brush) item->setBrush(colorFor(brush));
    if (pen) { auto p = item->pen(); p.setColor(colorFor(pen)); item->setPen(p); }
}
void text(QGraphicsScene *scene, const QString &value, QPointF point, int role, int size = 11) {
    auto *item = scene->addSimpleText(value, QFont("IBM Plex Sans", size));
    styleItem(item, role);
    item->setAcceptedMouseButtons(Qt::NoButton);
    item->setPos(point);
}
class TerminalDot : public QGraphicsEllipseItem {
public:
    TerminalDot(QPointF point, QGraphicsItem *parent)
        : QGraphicsEllipseItem(point.x() - 6, point.y() - 6, 12, 12, parent) {
        setCursor(Qt::CrossCursor);
    }
    std::function<void(Qt::KeyboardModifiers)> activated;
protected:
    void mousePressEvent(QGraphicsSceneMouseEvent *event) override {
        if (event->button() != Qt::LeftButton) { event->ignore(); return; }
        event->accept();
        if (activated) activated(event->modifiers());
    }
};
// Stored card locations survive project save/reload. Wires update after the drag.
class DeviceCard : public QGraphicsRectItem {
public:
    std::function<void(QPointF)> moved;
    std::function<void(QPointF)> committed;
    using QGraphicsRectItem::QGraphicsRectItem;
    void nudge(QPointF delta) { setPos(pos() + delta); if (committed) committed(pos()); }
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget) override {
        auto plain = *option;
        plain.state &= ~QStyle::State_Selected;
        QGraphicsRectItem::paint(painter, &plain, widget);
        if (isSelected()) {
            QPen focus(ThemeManager::instance()->tokens().focus, 2);
            focus.setCosmetic(true);
            painter->setPen(focus);
            painter->setBrush(Qt::NoBrush);
            painter->drawRect(rect().adjusted(1, 1, -1, -1));
        }
    }
    QVariant itemChange(GraphicsItemChange change, const QVariant &value) override {
        const auto result = QGraphicsRectItem::itemChange(change, value);
        if (change == ItemPositionHasChanged && moved) moved(value.toPointF());
        return result;
    }
protected:
    void mousePressEvent(QGraphicsSceneMouseEvent *event) override { dragStart = pos(); QGraphicsRectItem::mousePressEvent(event); }
    void mouseReleaseEvent(QGraphicsSceneMouseEvent *event) override {
        QGraphicsRectItem::mouseReleaseEvent(event);
        if (pos() != dragStart && committed) committed(pos());
    }
private:
    QPointF dragStart;
};
class CircuitView : public QGraphicsView {
public:
    using QGraphicsView::QGraphicsView;
    void fitCircuit() {
        ++viewportCommand;
        autoFit = true;
        setSceneRect(QRectF());
        fitInView(sceneRect(), Qt::KeepAspectRatio);
    }
    void zoomBy(qreal factor) {
        const qreal next = transform().m11() * factor;
        if (next < 0.15 || next > 5) return;
        ++viewportCommand;
        autoFit = false;
        scale(factor, factor);
    }
    void inspectSelection() {
        if (!scene() || scene()->selectedItems().isEmpty()) return;
        const QRectF card = scene()->selectedItems().first()->sceneBoundingRect();
        const qreal factor = 1.25; // Readable text even when a tall chip needs scrolling.
        const auto command = ++viewportCommand;
        autoFit = false;
        // Panning margins let edge components reach the viewport center.
        // They belong to this view; scene items and circuit data stay intact.
        const qreal marginX = viewport()->width() / (2 * factor) + 32;
        const qreal marginY = viewport()->height() / (2 * factor) + 32;
        setSceneRect(scene()->sceneRect().united(card)
                         .adjusted(-marginX, -marginY, marginX, marginY));
        QTransform readable;
        readable.scale(factor, factor);
        setTransform(readable);
        centerOn(card.center());
        // New scrollbars can post one last viewport resize. Center after it,
        // unless another user command has already changed the viewport.
        QTimer::singleShot(0, this, [this, command, center = card.center()]() {
            if (command == viewportCommand) centerOn(center);
        });
    }
protected:
    void resizeEvent(QResizeEvent *event) override {
        QGraphicsView::resizeEvent(event);
        if (autoFit) fitInView(sceneRect(), Qt::KeepAspectRatio);
    }
    void wheelEvent(QWheelEvent *event) override {
        if (event->modifiers() & Qt::ControlModifier) {
            zoomBy(event->angleDelta().y() > 0 ? 1.15 : 1 / 1.15);
            event->accept();
        } else {
            ++viewportCommand;
            autoFit = false;
            QGraphicsView::wheelEvent(event);
        }
    }
    void mouseMoveEvent(QMouseEvent *event) override {
        if (event->buttons() & Qt::LeftButton) { ++viewportCommand; autoFit = false; }
        QGraphicsView::mouseMoveEvent(event);
    }
    void mouseDoubleClickEvent(QMouseEvent *event) override {
        auto *item = itemAt(event->position().toPoint());
        while (item && !(item->flags() & QGraphicsItem::ItemIsSelectable)) item = item->parentItem();
        if (item) {
            scene()->clearSelection();
            item->setSelected(true);
            inspectSelection();
            event->accept();
        } else QGraphicsView::mouseDoubleClickEvent(event);
    }
    void keyPressEvent(QKeyEvent *event) override {
        if (event->key() == Qt::Key_Plus || event->key() == Qt::Key_Equal) { zoomBy(1.15); event->accept(); return; }
        if (event->key() == Qt::Key_Minus) { zoomBy(1 / 1.15); event->accept(); return; }
        if (event->key() == Qt::Key_0) { fitCircuit(); event->accept(); return; }
        if (event->key() == Qt::Key_I) { inspectSelection(); event->accept(); return; }
        QPointF movement;
        const int step = (event->modifiers() & Qt::ShiftModifier) ? 24 : 6;
        if (event->key() == Qt::Key_Left) movement.setX(-step);
        else if (event->key() == Qt::Key_Right) movement.setX(step);
        else if (event->key() == Qt::Key_Up) movement.setY(-step);
        else if (event->key() == Qt::Key_Down) movement.setY(step);
        if (!movement.isNull()) {
            ++viewportCommand;
            for (auto *item : scene()->selectedItems())
                if (auto *card = dynamic_cast<DeviceCard *>(item)) card->nudge(movement);
            event->accept();
            return;
        }
        QGraphicsView::keyPressEvent(event);
    }
private:
    bool autoFit = true;
    quint64 viewportCommand = 0;
};
}


namespace {
QJsonObject previewIdentity(QJsonObject json) { json.remove("ui_layout"); return json; }
QJsonObject electricalIdentity(QJsonObject json) { json.remove("geometry"); return json; }
QJsonArray componentJson(const ProjectDocument *project) { return project->toJson().value("components").toArray(); }
QJsonObject rawComponent(const ProjectDocument *project, const QString &id) {
    for (const auto &value : componentJson(project)) if (value.toObject().value("id").toString() == id) return value.toObject();
    return {};
}
std::optional<ProjectNet> connectedNet(const ProjectDocument *project, const QString &terminal) {
    for (const auto &net : project->nets()) if (net.endpoints.contains(terminal)) return net;
    return {};
}
int connectedGpio(const ProjectDocument *project, const QString &terminal) {
    const auto net = connectedNet(project, terminal);
    if (!net) return -1;
    QMap<QString, int> pads;
    for (const auto &component : project->components()) if (component.kind == "mcu")
        for (const auto &pad : component.terminals) if (pad.gpio) pads[pad.id] = *pad.gpio;
    QList<int> connected;
    for (const auto &id : net->endpoints) if (pads.contains(id)) connected.append(pads[id]);
    return connected.size() == 1 ? connected.first() : connected.isEmpty() ? -1 : -2;
}
QString chipTerminal(const ProjectDocument *project, int gpio) {
    for (const auto &component : project->components()) if (component.kind == "mcu")
        for (const auto &terminal : component.terminals) if (terminal.gpio && *terminal.gpio == gpio) return terminal.id;
    return {};
}
QString freshId(const QString &prefix) { return prefix + QUuid::createUuid().toString(QUuid::WithoutBraces); }
QString decoderLabel(const QJsonObject &decoder) {
    if (decoder.isEmpty()) return {};
    QStringList parts{decoder.value("controller").toString().toUpper()};
    if (decoder.contains("address")) parts.append("address " + decoder.value("address").toVariant().toString());
    if (decoder.contains("chip_select")) parts.append("CS " + decoder.value("chip_select").toVariant().toString());
    if (decoder.contains("unit")) parts.append("unit " + decoder.value("unit").toVariant().toString());
    return parts.join(" · ") + " (cached)";
}
QString parameterText(const QJsonValue &value) {
    if (value.isDouble()) return QString::number(value.toDouble(), 'g', 15);
    if (value.isString()) return value.toString();
    if (value.isBool()) return value.toBool() ? "true" : "false";
    return QString::fromUtf8(QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact));
}
void tone(QLabel *label, const char *value) {
    label->setProperty("tone", value);
    label->style()->unpolish(label); label->style()->polish(label); label->update();
}
}

BoardWorkspace::BoardWorkspace(QWidget *parent) : QWidget(parent), project(new ProjectDocument(this))
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(24, 24, 24, 16);
    auto *heading = new QLabel("Circuit workspace", this); heading->setObjectName("pageTitle"); root->addWidget(heading);
    auto *subtitle = new QLabel("Edit project connections and save them independently from the running simulator.", this);
    subtitle->setWordWrap(true); root->addWidget(subtitle);
    bridge = new QLabel(this); bridge->setObjectName("notice"); bridge->setWordWrap(true); root->addWidget(bridge);
    setBridgeAvailable(false);

    auto *files = new QHBoxLayout;
    auto *newButton = new QPushButton("New", this); newButton->setObjectName("newCircuit");
    auto *open = new QPushButton("Open circuit", this); open->setObjectName("openCircuit");
    auto *save = new QPushButton("Save", this); save->setObjectName("saveCircuit");
    auto *saveAs = new QPushButton("Save as…", this); saveAs->setObjectName("saveCircuitAs");
    applyButton = new QPushButton("Apply device preview", this); applyButton->setObjectName("applyCircuit");
    for (auto *button : {newButton, open, save, saveAs}) files->addWidget(button);
    auto *undo = project->undoStack()->createUndoAction(this, "Undo");
    auto *redo = project->undoStack()->createRedoAction(this, "Redo");
    undo->setShortcut(QKeySequence("Ctrl+Z")); redo->setShortcuts({QKeySequence("Ctrl+Y"), QKeySequence("Ctrl+Shift+Z")});
    for (auto *action : {undo, redo}) {
        action->setShortcutContext(Qt::WidgetWithChildrenShortcut); addAction(action);
        auto *button = new QToolButton(this); button->setDefaultAction(action); files->addWidget(button);
        const auto caption = action == undo ? QString("Undo") : QString("Redo");
        connect(action, &QAction::changed, button, [button, caption]() { button->setText(caption); });
        button->setText(caption);
    }
    nativeApplyButton = new QPushButton("Apply native circuit", this); nativeApplyButton->setObjectName("applyNativeCircuit");
    nativeApplyButton->setAccessibleName("Apply circuit to native electrical runtime");
    files->addStretch(); files->addWidget(applyButton); files->addWidget(nativeApplyButton); root->addLayout(files);
    applyStatus = new QLabel(this); applyStatus->setObjectName("applyStatus"); applyStatus->setWordWrap(true); root->addWidget(applyStatus);
    diagnosticView = new QLabel(this); diagnosticView->setObjectName("projectDiagnostics");
    diagnosticView->setWordWrap(true); diagnosticView->setTextInteractionFlags(Qt::TextSelectableByMouse); root->addWidget(diagnosticView);

    auto *toolbar = new QHBoxLayout;
    catalogue = new QComboBox(this); catalogue->setObjectName("componentCatalogue");
    catalogue->addItems({"SSD1306 OLED", "SHT21 sensor", "SSD1331 color OLED", "SPI flash", "UART loopback", "GPIO test bench", "ESP32-S3 logical chip",
        "Voltage source", "Current source", "Resistor", "Capacitor", "Potentiometer", "Switch", "Ground reference", "Button"});
    toolbar->addWidget(catalogue);
    auto *add = new QPushButton("Add component", this); add->setObjectName("addComponent"); toolbar->addWidget(add);
    wireButton = new QPushButton("Wire terminals", this); wireButton->setObjectName("wireTerminals"); wireButton->setCheckable(true);
    wireButton->setAccessibleName("Wire terminals by selecting two endpoints");
    wireButton->setToolTip("Click two terminal dots to connect or branch. Ctrl+click connects to the selected net; Alt+click disconnects.");
    toolbar->addWidget(wireButton);
    auto *zoomOut = new QPushButton("Zoom −", this); zoomOut->setAccessibleName("Zoom out"); zoomOut->setToolTip("Zoom out (Minus in canvas; Ctrl+wheel)");
    auto *zoomIn = new QPushButton("Zoom +", this); zoomIn->setAccessibleName("Zoom in"); zoomIn->setToolTip("Zoom in (Plus/Equal in canvas; Ctrl+wheel)");
    auto *fit = new QPushButton("Fit (0)", this); fit->setAccessibleName("Fit circuit");
    auto *inspect = new QPushButton("Inspect component", this); inspect->setObjectName("inspectComponent");
    inspect->setToolTip("Center at readable zoom (I in canvas; or double-click a component)");
    for (auto *button : {zoomOut, zoomIn, fit, inspect}) toolbar->addWidget(button);
    toolbar->addStretch(); root->addLayout(toolbar);

    auto *body = new QHBoxLayout;
    scene = new QGraphicsScene(this); scene->setSceneRect(0, 0, 1050, 700);
    canvas = new CircuitView(scene, this); canvas->setObjectName("circuitCanvas");
    canvas->setAccessibleName("Circuit canvas");
    canvas->setAccessibleDescription("Select components in the list or canvas. Arrow keys move; I inspects; plus/minus zoom; zero fits. Wire terminals then click two dots; Ctrl+click connects to the selected net; Alt+click disconnects. The Nets tab provides the complete keyboard equivalent.");
    canvas->setRenderHint(QPainter::Antialiasing); canvas->setResizeAnchor(QGraphicsView::AnchorViewCenter);
    canvas->setTransformationAnchor(QGraphicsView::AnchorUnderMouse); canvas->setDragMode(QGraphicsView::ScrollHandDrag); canvas->setMinimumSize(500, 400);
    body->addWidget(canvas, 1);
    auto *inspectorTabs = new QTabWidget(this); inspectorTabs->setObjectName("inspectorTabs"); inspectorTabs->setMinimumWidth(380);
    auto *componentPage = new QWidget(inspectorTabs); auto *inspector = new QVBoxLayout(componentPage);
    inspector->addWidget(new QLabel("COMPONENT CONNECTIONS", this));
    selection = new QComboBox(this); selection->setObjectName("selectedComponent"); selection->setMinimumWidth(260); inspector->addWidget(selection);
    pins = new QTableWidget(0, 2, this); pins->setObjectName("connectionTable");
    pins->setHorizontalHeaderLabels({"Terminal", "Connection"});
    auto *pinHeader = pins->horizontalHeader();
    // Terminal names size to their content; the connection column absorbs the rest.
    // A shared minimum section size keeps both header labels fully readable at any
    // inspector width instead of splitting the width equally until the text clips.
    pinHeader->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    pinHeader->setSectionResizeMode(1, QHeaderView::Stretch);
    int minimumSection = 0;
    for (const auto &label : {"Terminal", "Connection"}) {
        const auto text = QString::fromLatin1(label) + QStringLiteral("xx");
        minimumSection = qMax(minimumSection, pinHeader->fontMetrics().horizontalAdvance(text) + 24);
    }
    pinHeader->setMinimumSectionSize(minimumSection);
    pins->verticalHeader()->hide(); pins->setMinimumWidth(260); inspector->addWidget(pins, 1);
    auto *remove = new QPushButton("Remove component", this); remove->setObjectName("removeComponent"); inspector->addWidget(remove);
    decoderView = new QLabel(componentPage); decoderView->setObjectName("cachedDecoder"); decoderView->setWordWrap(true); decoderView->setTextFormat(Qt::PlainText); inspector->addWidget(decoderView);
    auto *parameterArea = new QScrollArea(componentPage); parameterArea->setObjectName("quantityEditors"); parameterArea->setWidgetResizable(true); parameterArea->setMaximumHeight(230);
    auto *parameterPanel = new QWidget(parameterArea); quantities = new QFormLayout(parameterPanel); parameterArea->setWidget(parameterPanel); inspector->addWidget(parameterArea);
    auto *hint = new QLabel("Shared legacy I2C/SPI edits move the matching terminal branches together. Lines show declared nets, not measured levels. Controller/address labels are cached decoder metadata.", this);
    hint->setWordWrap(true); inspector->addWidget(hint);
    auto *componentScroll = new QScrollArea(inspectorTabs); componentScroll->setObjectName("componentInspectorScroll");
    componentScroll->setWidgetResizable(true); componentScroll->setFrameShape(QFrame::NoFrame);
    componentScroll->setWidget(componentPage); inspectorTabs->addTab(componentScroll, "Component");
    auto *netPage = new QWidget(inspectorTabs); auto *netLayout = new QVBoxLayout(netPage);
    auto *netFields = new QFormLayout;
    netSelection = new QComboBox(netPage); netSelection->setObjectName("netSelector"); netFields->addRow("Net", netSelection);
    netName = new QLineEdit(netPage); netName->setObjectName("netName"); netName->setAccessibleName("Net display name"); netFields->addRow("Name", netName);
    auto *driverProfile = new QComboBox(netPage); driverProfile->setObjectName("electricalDriverProfile");
    driverProfile->setAccessibleName("Native electrical driver impedance profile");
    driverProfile->addItem("Choose explicit driver profile", "");
    driverProfile->addItem("S3 finite model: 40 Ω drive / 45 kΩ pull", "s3-explicit-finite-v1");
    auto *solveMode = new QComboBox(netPage); solveMode->setObjectName("electricalSolveMode"); solveMode->setAccessibleName("Native electrical solve mode");
    solveMode->addItem("DC", "dc"); solveMode->addItem("RC", "rc");
    auto *chargePolicy = new QComboBox(netPage); chargePolicy->setObjectName("electricalChargePolicy"); chargePolicy->setAccessibleName("Charge policy on RC edits");
    chargePolicy->addItem("Choose RC edit charge policy", ""); chargePolicy->addItem("Reset charge", "reset"); chargePolicy->addItem("Keep charge", "keep");
    netFields->addRow("Driver model", driverProfile); netFields->addRow("Solver", solveMode); netFields->addRow("RC edits", chargePolicy);
    const auto setElectricalProfile = [this, driverProfile, solveMode, chargePolicy]() {
        auto runtime = project->runtime(); auto electrical = runtime.value("electrical").toObject();
        if (driverProfile->currentData().toString().isEmpty()) electrical.remove("driver_profile");
        else electrical["driver_profile"] = driverProfile->currentData().toString();
        electrical["mode"] = solveMode->currentData().toString();
        if (chargePolicy->currentData().toString().isEmpty()) electrical.remove("edit_charge");
        else electrical["edit_charge"] = chargePolicy->currentData().toString();
        runtime["electrical"] = electrical; project->setRuntime(runtime);
    };
    for (auto *control : {driverProfile, solveMode, chargePolicy})
        connect(control, qOverload<int>(&QComboBox::activated), this, setElectricalProfile);
    netLayout->addLayout(netFields);
    auto *netActions = new QHBoxLayout;
    auto *createNet = new QPushButton("Create net", netPage); createNet->setObjectName("createNet");
    auto *renameNet = new QPushButton("Rename", netPage); renameNet->setObjectName("renameNet");
    auto *removeNet = new QPushButton("Remove", netPage); removeNet->setObjectName("removeNet");
    for (auto *button : {createNet, renameNet, removeNet}) netActions->addWidget(button);
    netLayout->addLayout(netActions);
    terminalSelection = new QComboBox(netPage); terminalSelection->setObjectName("terminalSelector"); terminalSelection->setAccessibleName("Terminal to connect"); netLayout->addWidget(terminalSelection);
    terminalName = new QLineEdit(netPage); terminalName->setObjectName("terminalName"); terminalName->setAccessibleName("Terminal display name"); netLayout->addWidget(terminalName);
    auto *renameTerminal = new QPushButton("Rename terminal", netPage); renameTerminal->setObjectName("renameTerminal"); netLayout->addWidget(renameTerminal);
    auto *connectTerminal = new QPushButton("Connect to net", netPage); connectTerminal->setObjectName("connectTerminal"); netLayout->addWidget(connectTerminal);
    netMembers = new QListWidget(netPage); netMembers->setObjectName("netMembers"); netMembers->setAccessibleName("Net terminal members"); netLayout->addWidget(netMembers, 1);
    auto *disconnectTerminal = new QPushButton("Disconnect selected", netPage); disconnectTerminal->setObjectName("disconnectTerminal"); netLayout->addWidget(disconnectTerminal);
    netInfo = new QLabel(netPage); netInfo->setObjectName("netInfo"); netInfo->setWordWrap(true); netInfo->setTextFormat(Qt::PlainText); netLayout->addWidget(netInfo);
    electricalAvailability = new QLabel("Native electrical measurements unavailable. Declared values are not readings.", netPage);
    electricalAvailability->setObjectName("electricalAvailability"); electricalAvailability->setWordWrap(true); electricalAvailability->setProperty("tone", "warning"); netLayout->addWidget(electricalAvailability);
    auto *netScroll = new QScrollArea(inspectorTabs); netScroll->setObjectName("netInspectorScroll");
    netScroll->setWidgetResizable(true); netScroll->setFrameShape(QFrame::NoFrame);
    netScroll->setWidget(netPage); inspectorTabs->addTab(netScroll, "Nets");
    body->addWidget(inspectorTabs); root->addLayout(body, 1);
    message = new QLabel(this); message->setObjectName("projectStatus"); message->setWordWrap(true); root->addWidget(message);
    catalogue->setAccessibleName("Component catalogue"); selection->setAccessibleName("Selected component");
    pins->setAccessibleName("Component terminal connections"); netSelection->setAccessibleName("Selected net");
    netName->setToolTip("Display name only; connections use immutable terminal IDs.");
    terminalSelection->setToolTip("Select an actual terminal ID, then Connect to net. Disconnect selected removes only that branch.");
    auto *disconnectAction = new QAction(netMembers); disconnectAction->setShortcut(QKeySequence(Qt::Key_Delete));
    disconnectAction->setShortcutContext(Qt::WidgetShortcut); netMembers->addAction(disconnectAction);
    connect(disconnectAction, &QAction::triggered, disconnectTerminal, &QPushButton::click);
    connect(netName, &QLineEdit::returnPressed, renameNet, &QPushButton::click);
    connect(terminalName, &QLineEdit::returnPressed, renameTerminal, &QPushButton::click);
    connect(wireButton, &QPushButton::toggled, this, [this]() { pendingTerminal.clear(); });
    connect(pins, &QTableWidget::cellClicked, this, [this](int row, int) {
        if (pins->item(row, 0)) selectTerminal(pins->item(row, 0)->data(TerminalRole).toString());
    });

    connect(project, &ProjectDocument::documentChanged, this, &BoardWorkspace::scheduleRefresh);
    connect(project, &ProjectDocument::dirtyChanged, this, [this]() { updateDocumentStatus(); });
    connect(selection, qOverload<int>(&QComboBox::currentIndexChanged), this, [this]() { rebuildPins(); syncCanvasSelection(); });
    connect(netSelection, qOverload<int>(&QComboBox::currentIndexChanged), this, &BoardWorkspace::updateNetDetails);
    connect(terminalSelection, qOverload<int>(&QComboBox::currentIndexChanged), this, [this]() {
        const auto id = terminalSelection->currentData().toString();
        for (const auto &component : project->components()) for (const auto &terminal : component.terminals) if (terminal.id == id) terminalName->setText(terminal.name);
    });
    connect(netMembers, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *item) { if (item) { const int index = terminalSelection->findData(item->data(Qt::UserRole)); if (index >= 0) terminalSelection->setCurrentIndex(index); } });
    connect(createNet, &QPushButton::clicked, this, [this]() {
        const auto id = project->addNet({{"name", netName->text().isEmpty() ? QString("Net %1").arg(project->nets().size() + 1) : netName->text()}});
        rebuildNetEditor(); netSelection->setCurrentIndex(netSelection->findData(id));
    });
    connect(renameNet, &QPushButton::clicked, this, [this]() { QString error; if (!project->renameNet(netSelection->currentData().toString(), netName->text(), &error)) showEditError(error); });
    connect(removeNet, &QPushButton::clicked, this, [this]() { QString error; if (!project->removeNet(netSelection->currentData().toString(), &error)) showEditError(error); });
    connect(renameTerminal, &QPushButton::clicked, this, [this]() { QString error; if (!project->renameTerminal(terminalSelection->currentData().toString(), terminalName->text(), &error)) showEditError(error); });
    connect(connectTerminal, &QPushButton::clicked, this, [this]() { QString error; if (!project->connectTerminal(netSelection->currentData().toString(), terminalSelection->currentData().toString(), &error)) showEditError(error); });
    connect(disconnectTerminal, &QPushButton::clicked, this, [this]() {
        const auto id = netMembers->currentItem() ? netMembers->currentItem()->data(Qt::UserRole).toString() : terminalSelection->currentData().toString();
        const auto current = connectedNet(project, id);
        QString error; if (!project->disconnectTerminal(current ? current->id : netSelection->currentData().toString(), id, &error)) showEditError(error);
    });
    connect(scene, &QGraphicsScene::selectionChanged, this, [this]() {
        if (scene->selectedItems().isEmpty()) return;
        const int index = selection->findData(scene->selectedItems().first()->data(ComponentRole));
        if (index >= 0 && index != selection->currentIndex()) selection->setCurrentIndex(index);
    });
    connect(zoomOut, &QPushButton::clicked, this, [this]() { static_cast<CircuitView *>(canvas)->zoomBy(1 / 1.15); });
    connect(zoomIn, &QPushButton::clicked, this, [this]() { static_cast<CircuitView *>(canvas)->zoomBy(1.15); });
    connect(fit, &QPushButton::clicked, this, [this]() { static_cast<CircuitView *>(canvas)->fitCircuit(); });
    connect(inspect, &QPushButton::clicked, this, [this]() { static_cast<CircuitView *>(canvas)->inspectSelection(); });
    connect(ThemeManager::instance(), &ThemeManager::themeChanged, this, &BoardWorkspace::updateCanvasTheme);
    connect(add, &QPushButton::clicked, this, &BoardWorkspace::addDevice);
    connect(remove, &QPushButton::clicked, this, &BoardWorkspace::removeDevice);
    connect(save, &QPushButton::clicked, this, [this]() { saveProject(); });
    connect(saveAs, &QPushButton::clicked, this, [this]() { saveProject(true); });
    connect(applyButton, &QPushButton::clicked, this, [this]() { QString error; if (!applyCircuit(&error)) { applyStatus->setText(error); tone(applyStatus, "warning"); } });
    connect(nativeApplyButton, &QPushButton::clicked, this, [this]() {
        QString error; if (!requestNativeApply(&error)) showEditError(error);
    });
    connect(open, &QPushButton::clicked, this, [this]() {
        if (!confirmDiscard()) return;
        const auto path = QFileDialog::getOpenFileName(this, "Open project", project->filePath(), "Project (*.json)");
        if (!path.isEmpty()) { QString error; if (!openCircuit(path, &error)) { message->setText("Open failed: " + error); tone(message, "error"); } }
    });
    connect(newButton, &QPushButton::clicked, this, [this]() { if (confirmDiscard()) newCircuit(); });
    auto *saveAction = new QAction(this); saveAction->setShortcut(QKeySequence("Ctrl+S")); saveAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    addAction(saveAction); connect(saveAction, &QAction::triggered, this, [this]() { saveProject(); });
    rebuild();
}
BoardWorkspace::~BoardWorkspace() {
    // QUndoStack destruction emits cleanChanged/dirtyChanged while QWidget is
    // deleting children. Stop document callbacks before inspectors disappear.
    disconnect(project, nullptr, this, nullptr);
    delete runtimeConfigDirectory;
}
QJsonObject BoardWorkspace::circuitDocument() const { return project->toJson(); }
void BoardWorkspace::scheduleRefresh() {
    if (refreshQueued) return;
    refreshQueued = true;
    QTimer::singleShot(0, this, [this]() { if (!refreshQueued) return; refreshQueued = false; rebuild(); });
}
void BoardWorkspace::setManager(PeripheralManager *value) {
    if (manager) disconnect(manager, nullptr, this, nullptr);
    manager = value;
    if (!manager) { updateDocumentStatus(); return; }
    connect(manager, &PeripheralManager::deviceSetChanged, this, [this]() {
        if (manager->configPath() != appliedConfigPath) hasAppliedPreview = false;
        updateDocumentStatus(); // runtime changes never replace the editable project
    });
    loadCurrentConfig();
}
void BoardWorkspace::loadCurrentConfig() {
    if (!manager || manager->configPath().isEmpty()) return;
    QString error;
    if (!openCircuit(manager->configPath(), &error)) { message->setText("Project import failed: " + error); return; }
    const auto preview = project->legacyRuntimeJsonWithAbsoluteResources(&error);
    if (!preview.isEmpty()) { appliedPreview = previewIdentity(preview); appliedConfigPath = manager->configPath(); hasAppliedPreview = true; }
    updateDocumentStatus();
}
void BoardWorkspace::setBridgeAvailable(bool available) {
    bridge->setText(available
        ? "Legacy controller/address preview transport available; it does not route electrical nets. Native circuit support is independently acknowledged."
        : "Legacy controller/address preview transport unavailable. Native circuit support and measurements are independently acknowledged.");
}
void BoardWorkspace::setRuntimeStatus(const RuntimeStatus &status) {
    if (runtimeStatus.sessionId != status.sessionId || runtimeStatus.resetEpoch != status.resetEpoch) {
        nativeCircuitSnapshot = {}; nativeSnapshotIdentity = {}; nativeApplyResult.clear(); nativeApplyInFlight = false;
        if (runtimeStatus.sessionId != status.sessionId) nativeCircuitAvailable = false;
    }
    runtimeStatus = status; runtimeStatusKnown = true;
    if (status.phase == RuntimePhase::Idle || status.phase == RuntimePhase::Stopped || status.phase == RuntimePhase::Error) {
        nativeCircuitAvailable = false; nativeCircuitSnapshot = {}; nativeSnapshotIdentity = {};
    }
    updateDocumentStatus(); updateNetDetails();
}
bool BoardWorkspace::openCircuit(const QString &path, QString *error) {
    QFile file(path); if (!file.open(QIODevice::ReadOnly)) { if (error) *error = file.errorString(); return false; }
    const auto raw = QJsonDocument::fromJson(file.readAll()).object();
    const auto previousId = project->toJson().value("id");
    const bool wasEmpty = project->components().isEmpty();
    if (!project->loadFile(path, error)) return false;
    importedV2 = raw.value("version").toInt() == 2;
    pendingTerminal.clear(); cacheCatalogue(path); refreshQueued = false; rebuild();
    if (wasEmpty || previousId != project->toJson().value("id")) static_cast<CircuitView *>(canvas)->fitCircuit();
    return true;
}
void BoardWorkspace::newCircuit() {
    ProjectDocument empty; project->loadJson(empty.toJson());
    pendingTerminal.clear();
    importedV2 = false; refreshQueued = false; rebuild(); static_cast<CircuitView *>(canvas)->fitCircuit();
}
void BoardWorkspace::cacheCatalogue(const QString &path) {
    const auto directory = QFileInfo(path).absolutePath();
    project->setLegacyResourceRoot(QDir(directory + "/..").absolutePath());
    for (const auto &name : {"peripherals.example.json", "peripherals.ssd1331_sht21.example.json"}) {
        const auto source = QDir(directory).absoluteFilePath(name);
        ProjectDocument catalogueProject;
        if (!catalogueProject.loadFile(source)) continue;
        const auto raw = componentJson(&catalogueProject); const auto typed = catalogueProject.components();
        for (int i = 0; i < raw.size(); ++i) {
            auto entry = raw[i].toObject(); if (entry.value("kind") != "device") continue;
            const auto found = std::find_if(typed.begin(), typed.end(), [&](const ProjectComponent &c) { return c.id == entry.value("id").toString(); });
            if (found == typed.end()) continue;
            entry["simulator"] = found->simulator; entry["attributes"] = found->attributes;
            auto originalDevice = entry.value("legacy").toObject();
            originalDevice["simulator"] = found->simulator;
            if (originalDevice.contains("properties")) originalDevice["properties"] = found->attributes.value("properties");
            entry["catalogue"] = QJsonObject{{"source_project_path", source}, {"template_device", originalDevice}};
            templates[entry.value("type").toString()] = entry;
        }
    }
}
bool BoardWorkspace::confirmDiscard() {
    if (!project->isDirty()) return true;
    const auto choice = QMessageBox::question(this, "Unsaved project", "Save your project before replacing it?",
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (choice == QMessageBox::Cancel) return false;
    if (choice == QMessageBox::Discard) return true;
    saveProject(); return !project->isDirty();
}
void BoardWorkspace::saveProject(bool saveAs) {
    QString path = project->filePath();
    if (saveAs || importedV2 || path.isEmpty()) {
        const auto suggestion = importedV2 ? QFileInfo(path).absolutePath() + "/" + QFileInfo(path).completeBaseName() + ".project.json" : path;
        path = QFileDialog::getSaveFileName(this, "Save project", suggestion, "Project (*.json)");
        if (path.isEmpty()) return;
    }
    saveCircuit(path);
}
bool BoardWorkspace::saveCircuit(const QString &path) {
    QString error;
    if (!project->saveAs(path, &error)) { message->setText("Save failed: " + error); tone(message, "error"); return false; }
    importedV2 = false; updateDocumentStatus(); return true;
}
QString BoardWorkspace::applyUnavailableReason() const {
    if (!manager) return "No device preview runtime is attached.";
    if (!runtimeStatusKnown) return "Runtime state is not known. Apply remains disabled.";
    if (runtimeStatus.phase != RuntimePhase::Idle && runtimeStatus.phase != RuntimePhase::Stopped) return "Stop the simulator before applying device configuration.";
    for (const auto &diagnostic : project->diagnostics()) if (diagnostic.code == "resource-host-mapping-required" || diagnostic.code == "resource-network-access-unverified")
        return "Map or verify network/other-host resource paths before applying them.";
    QString reason;
    if (project->legacyRuntimeJsonWithAbsoluteResources(&reason).isEmpty()) return reason;
    return {};
}
bool BoardWorkspace::applyCircuit(QString *error) {
    const auto unavailable = applyUnavailableReason();
    if (!unavailable.isEmpty()) { if (error) *error = unavailable; return false; }
    auto preview = project->legacyRuntimeJsonWithAbsoluteResources(error);
    const auto identity = previewIdentity(preview);
    if (hasAppliedPreview && manager->configPath() == appliedConfigPath && identity == appliedPreview) {
        applyStatus->setText("Device preview already matches. No processes restarted. Physical routing remains unavailable.");
        tone(applyStatus, "info"); if (error) error->clear(); return true;
    }
    auto *directory = new QTemporaryDir;
    if (!directory->isValid()) { delete directory; if (error) *error = "Could not create device preview directory."; return false; }
    const auto path = directory->filePath("device-preview.json");
    QSaveFile file(path); file.setDirectWriteFallback(false);
    const auto bytes = QJsonDocument(preview).toJson(QJsonDocument::Indented);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        if (error) *error = file.errorString();
        delete directory; return false;
    }
    if (!manager->loadConfig(path)) { if (error) *error = "Device preview validation failed; the previous runtime is retained."; delete directory; return false; }
    delete runtimeConfigDirectory; runtimeConfigDirectory = directory;
    appliedConfigPath = manager->configPath(); appliedPreview = identity; hasAppliedPreview = true;
    updateDocumentStatus();
    applyStatus->setText("Device preview configuration loaded. Check startup status in Peripherals. Physical pin routing remains unavailable.");
    tone(applyStatus, "info"); if (error) error->clear(); return true;
}
void BoardWorkspace::setNativeCircuitAvailable(bool available, const QString &reason) {
    nativeCircuitAvailable = available; nativeCircuitReason = reason;
    if (!available) { nativeCircuitSnapshot = {}; nativeSnapshotIdentity = {}; }
    electricalAvailability->setText(available ? "Native electrical runtime confirmed. Readings require an accepted circuit snapshot."
        : "Native electrical runtime unavailable. " + reason);
    tone(electricalAvailability, available ? "info" : "warning");
    updateDocumentStatus(); updateNetDetails();
}
void BoardWorkspace::setNativeCircuitSnapshot(const QJsonObject &snapshot, const QJsonObject &acceptedDocument) {
    nativeCircuitSnapshot = snapshot.value("abi").toInt() == 1 ? snapshot : QJsonObject{};
    nativeSnapshotIdentity = electricalIdentity(acceptedDocument);
    updateNetDetails();
}
void BoardWorkspace::setNativeApplyInFlight(bool inFlight, const QString &result) {
    nativeApplyInFlight = inFlight; nativeApplyResult = result; updateDocumentStatus();
}
QString BoardWorkspace::nativeApplyUnavailableReason() const {
    if (!nativeCircuitAvailable) return nativeCircuitReason.isEmpty() ? "Native electrical runtime support has not been confirmed." : nativeCircuitReason;
    if (!runtimeStatusKnown) return "Runtime state is not known.";
    if (nativeApplyInFlight) return "Native Apply is in flight; awaiting QMP acknowledgement.";
    if (runtimeStatus.phase != RuntimePhase::Paused && runtimeStatus.phase != RuntimePhase::Initializing)
        return "Pause the live simulator and await acknowledgement before native Apply (no machine exists while stopped).";
    const auto electrical = project->runtime().value("electrical").toObject();
    if (electrical.value("driver_profile").toString() != "s3-explicit-finite-v1") return "Choose the explicit S3 finite driver impedance profile in Nets.";
    const auto mode = electrical.value("mode").toString("dc");
    if (mode != "dc" && mode != "rc") return "Choose DC or RC solver mode in Nets.";
    if (mode == "rc" && electrical.value("edit_charge").toString() != "reset" && electrical.value("edit_charge").toString() != "keep")
        return "Choose an explicit RC edit charge policy in Nets.";
    for (const auto &diagnostic : project->diagnostics()) if (diagnostic.severity == ProjectDiagnostic::Severity::Error)
        return diagnostic.path + ": " + diagnostic.message;
    return {};
}
bool BoardWorkspace::requestNativeApply(QString *error) {
    const auto reason = nativeApplyUnavailableReason();
    if (!reason.isEmpty()) { if (error) *error = reason; return false; }
    if (error) error->clear();
    setNativeApplyInFlight(true);
    emit nativeApplyRequested(project->toJson());
    if (nativeApplyInFlight) message->setText("Native Apply requested; awaiting QMP acknowledgement. The saved draft is unchanged.");
    return true;
}
void BoardWorkspace::updateDocumentStatus() {
    if (!applyButton || !message) return;
    const auto reason = applyUnavailableReason();
    applyButton->setEnabled(reason.isEmpty()); applyButton->setToolTip(reason.isEmpty() ? "Apply compatible controller/address device configuration; this does not route firmware pins." : reason);
    const auto nativeReason = nativeApplyUnavailableReason();
    nativeApplyButton->setEnabled(nativeReason.isEmpty());
    nativeApplyButton->setToolTip(nativeReason.isEmpty() ? "Apply the pure v3 graph to the acknowledged paused machine; QMP acceptance is required." : nativeReason);
    applyStatus->setText((reason.isEmpty() ? QString("Legacy preview available.") : "Legacy Apply unavailable: " + reason)
        + "\n" + (nativeReason.isEmpty() ? QString("Native Apply available at the acknowledged paused boundary.") : "Native Apply unavailable: " + nativeReason)
        + (nativeApplyResult.isEmpty() ? QString{} : "\n" + nativeApplyResult));
    tone(applyStatus, nativeReason.isEmpty() || reason.isEmpty() ? "info" : "warning");
    int errors = 0, warnings = 0; QStringList details;
    for (const auto &diagnostic : project->diagnostics()) {
        if (diagnostic.severity == ProjectDiagnostic::Severity::Error) ++errors; else ++warnings;
        details.append(diagnostic.code + ": " + diagnostic.message);
    }
    diagnosticView->setText(QString("%1 error(s), %2 warning(s).%3").arg(errors).arg(warnings).arg(errors ? " Invalid drafts can still be saved." : ""));
    diagnosticView->setToolTip(details.join("\n")); tone(diagnosticView, errors ? "error" : warnings ? "warning" : "helper");
    const auto path = project->filePath().isEmpty() ? QString("Untitled project") : project->filePath();
    message->setText((project->isDirty() ? "Unsaved changes · " : "Saved · ") + path + (importedV2 ? " · Imported v2; Save creates a v3 copy." : ""));
    const auto electrical = project->runtime().value("electrical").toObject();
    for (const auto &setting : {qMakePair(QString("electricalDriverProfile"), QString("driver_profile")),
                               qMakePair(QString("electricalSolveMode"), QString("mode")),
                               qMakePair(QString("electricalChargePolicy"), QString("edit_charge"))}) {
        auto *control = findChild<QComboBox *>(setting.first); QSignalBlocker blocker(control);
        const auto value = electrical.value(setting.second).toString(setting.second == "mode" ? "dc" : "");
        int index = control->findData(value);
        if (index < 0) { control->addItem("Unsupported: " + value, value); index = control->count() - 1; }
        control->setCurrentIndex(index);
    }
    tone(message, "helper");
}
void BoardWorkspace::rebuild() {
    const auto selectedId = selection->currentData().toString();
    { QSignalBlocker blocker(selection); selection->clear();
      const auto components = project->components();
      for (const auto &component : components) if (component.kind != "mcu") selection->addItem(component.name + " / " + component.type, component.id);
      for (const auto &component : components) if (component.kind == "mcu") selection->addItem(component.name + " / chip", component.id);
      const int index = selection->findData(selectedId); selection->setCurrentIndex(index >= 0 ? index : selection->count() ? 0 : -1);
    }
    rebuildPins(); rebuildNetEditor(); rebuildCanvas(); updateDocumentStatus();
}
void BoardWorkspace::rebuildPins() {
    pins->setRowCount(0);
    const auto id = selection->currentData().toString();
    const auto components = project->components();
    const auto found = std::find_if(components.begin(), components.end(), [&](const ProjectComponent &component) { return component.id == id; });
    if (found == components.end()) { decoderView->clear(); rebuildQuantities({}); return; }
    decoderView->setText(decoderLabel(found->attributes.value("decoder").toObject()));
    for (const auto &terminal : found->terminals) {
        const int row = pins->rowCount(); pins->insertRow(row);
        auto *name = new QTableWidgetItem(terminal.name); name->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        name->setData(TerminalRole, terminal.id); name->setToolTip(terminal.id + " · " + terminal.domain + " · " + terminal.direction); pins->setItem(row, 0, name);
        const auto net = connectedNet(project, terminal.id);
        if (found->kind == "mcu") {
            auto *label = new QLabel(terminal.gpio ? QString("GPIO %1 · %2").arg(*terminal.gpio).arg(net ? net->name : "no net") : net ? net->name : "no net", pins);
            label->setWordWrap(true); pins->setCellWidget(row, 1, label); continue;
        }
        if (found->kind != "device") {
            auto *label = new QLabel(net ? net->name : "No net (select terminal → Nets)", pins); label->setWordWrap(true);
            label->setToolTip(terminal.id); pins->setCellWidget(row, 1, label); continue;
        }
        auto *gpio = new QComboBox(pins); gpio->addItem("Disconnected from GPIO", -1);
        for (int number = 0; number <= 48; ++number) if (validGpio(number) && !chipTerminal(project, number).isEmpty()) gpio->addItem(QString("GPIO %1").arg(number), number);
        const int connected = connectedGpio(project, terminal.id);
        if (connected == -2) { gpio->addItem("Net spans multiple chip pads", -2); gpio->setEnabled(false); }
        gpio->setCurrentIndex(gpio->findData(connected));
        gpio->setToolTip(net ? "Declared net: " + net->id + " / " + net->name : "No declared net. Cached decoder data does not create a connection.");
        pins->setCellWidget(row, 1, gpio);
        connect(gpio, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, gpio, id, terminalId = terminal.id]() { changePin(id, terminalId, gpio->currentData().toInt()); });
    }
    rebuildQuantities(id);
    sizePinRows();
}
void BoardWorkspace::showEditError(const QString &error) {
    message->setText(error); tone(message, "warning");
}
void BoardWorkspace::rebuildNetEditor() {
    const auto selectedNet = netSelection->currentData().toString(); const auto selectedTerminal = terminalSelection->currentData().toString();
    { QSignalBlocker blockNet(netSelection), blockTerminal(terminalSelection);
        netSelection->clear(); terminalSelection->clear();
        for (const auto &net : project->nets()) netSelection->addItem(net.name + QString(" · %1 terminal(s)").arg(net.endpoints.size()), net.id);
        for (const auto &component : project->components()) for (const auto &terminal : component.terminals)
            terminalSelection->addItem(component.name + " / " + terminal.name, terminal.id);
        const int netIndex = netSelection->findData(selectedNet), terminalIndex = terminalSelection->findData(selectedTerminal);
        netSelection->setCurrentIndex(netIndex >= 0 ? netIndex : netSelection->count() ? 0 : -1);
        terminalSelection->setCurrentIndex(terminalIndex >= 0 ? terminalIndex : terminalSelection->count() ? 0 : -1);
    }
    const auto id = terminalSelection->currentData().toString(); terminalName->clear();
    for (const auto &component : project->components()) for (const auto &terminal : component.terminals) if (terminal.id == id) terminalName->setText(terminal.name);
    updateNetDetails();
}
void BoardWorkspace::updateNetDetails() {
    const auto id = netSelection->currentData().toString();
    QMap<QString, QString> names;
    for (const auto &component : project->components()) for (const auto &terminal : component.terminals) names[terminal.id] = component.name + " / " + terminal.name;
    QSignalBlocker blocker(netMembers); netMembers->clear();
    for (const auto &net : project->nets()) if (net.id == id) {
        netName->setText(net.name);
        for (const auto &endpoint : net.endpoints) {
            auto *item = new QListWidgetItem(names.contains(endpoint) ? names[endpoint] : "Missing terminal: " + endpoint, netMembers);
            item->setData(Qt::UserRole, endpoint); item->setToolTip(endpoint);
            if (endpoint == terminalSelection->currentData().toString()) netMembers->setCurrentItem(item);
        }
        updateNetHighlight();
        QString reading = "unavailable";
        if (nativeCircuitAvailable && !nativeCircuitSnapshot.isEmpty()) {
            if (nativeSnapshotIdentity != electricalIdentity(project->toJson())) reading = "stale — draft differs from the accepted native graph";
            else {
                reading = "unavailable — no native reading for this net";
                for (const auto &entry : nativeCircuitSnapshot.value("nets").toArray()) {
                    const auto sample = entry.toObject(); if (sample.value("id").toString() != net.id) continue;
                    if (sample.value("valid").toBool() && sample.value("voltage_v").isDouble())
                        reading = QString("%1 V · valid").arg(sample.value("voltage_v").toDouble(), 0, 'g', 9);
                    else reading = sample.value("floating").toBool() ? "floating · voltage unavailable" : "indeterminate · voltage unavailable";
                    break;
                }
                reading += QString("\nBackend state: %1\nGeneration %2 · virtual time %3 ns\n%4")
                    .arg(nativeCircuitSnapshot.value("status").toString("unavailable"),
                         nativeCircuitSnapshot.value("generation").toVariant().toString(),
                         nativeCircuitSnapshot.value("timestamp_ns").toVariant().toString(),
                         nativeCircuitSnapshot.value("diagnostic").toString());
            }
        }
        netInfo->setText(QString("%1 endpoint(s). Net ID: %2\nElectrical state: %3").arg(net.endpoints.size()).arg(net.id, reading));
        return;
    }
    netName->clear(); netInfo->setText("Create a net, then connect terminals. A terminal belongs to one net; disconnect it before moving it to another.");
    updateNetHighlight();
}
void BoardWorkspace::selectTerminal(const QString &id) {
    const int index = terminalSelection->findData(id); if (index < 0) return;
    terminalSelection->setCurrentIndex(index);
    if (const auto net = connectedNet(project, id)) netSelection->setCurrentIndex(netSelection->findData(net->id));
    findChild<QTabWidget *>("inspectorTabs")->setCurrentIndex(1);
    updateNetDetails();
}
void BoardWorkspace::activateTerminal(const QString &id, Qt::KeyboardModifiers modifiers) {
    const auto selectedNet = netSelection->currentData().toString();
    if (modifiers.testFlag(Qt::AltModifier)) {
        if (const auto net = connectedNet(project, id)) project->disconnectTerminal(net->id, id);
        pendingTerminal.clear();
    } else if (modifiers.testFlag(Qt::ControlModifier)) {
        QString error; if (!project->connectTerminal(selectedNet, id, &error)) showEditError(error);
    } else if (wireButton->isChecked()) {
        if (pendingTerminal.isEmpty()) {
            pendingTerminal = id; message->setText("First endpoint selected. Click another terminal to connect; toggle Wire terminals to cancel.");
        } else {
            const auto first = pendingTerminal; pendingTerminal.clear();
            if (first != id) {
                const auto a = connectedNet(project, first), b = connectedNet(project, id);
                if (a && b && a->id != b->id) showEditError("Endpoints belong to different nets. Disconnect the intended branch before reconnecting.");
                else if (!a || !b) {
                    project->undoStack()->beginMacro("Wire terminals");
                    const auto netId = a ? a->id : b ? b->id : project->addNet({{"name", QString("Net %1").arg(project->nets().size() + 1)}});
                    QString error;
                    if ((!a && !project->connectTerminal(netId, first, &error))
                        || (!b && !project->connectTerminal(netId, id, &error))) showEditError(error);
                    project->undoStack()->endMacro();
                }
            }
        }
    }
    selectTerminal(id);
}
void BoardWorkspace::updateNetHighlight() {
    const auto id = netSelection->currentData().toString();
    for (auto *item : scene->items()) {
        if (auto *path = dynamic_cast<QGraphicsPathItem *>(item)) {
            auto pen = path->pen(); pen.setWidthF(item->data(NetRole).toString() == id ? 3.2 : 1.6); path->setPen(pen);
        }
        if (auto *dot = dynamic_cast<TerminalDot *>(item)) {
            const auto net = connectedNet(project, item->data(TerminalRole).toString());
            auto pen = dot->pen(); pen.setWidthF(net && net->id == id ? 3 : 1); dot->setPen(pen);
        }
    }
}
void BoardWorkspace::rebuildQuantities(const QString &componentId) {
    quantityRefreshing = true;
    while (quantities->count()) { auto *item = quantities->takeAt(0); if (item->widget()) delete item->widget(); delete item; }
    const auto parameters = rawComponent(project, componentId).value("parameters").toObject();
    if (parameters.isEmpty()) { quantities->addRow(new QLabel("No declared quantities for this component.", this)); quantityRefreshing = false; return; }
    for (auto it = parameters.begin(); it != parameters.end(); ++it) {
        const auto name = it.key(); const auto quantity = it.value().toObject(); const auto unit = quantity.value("unit").toString();
        auto *row = new QWidget(this); auto *layout = new QHBoxLayout(row); layout->setContentsMargins(0, 0, 0, 0);
        auto *apply = new QPushButton("Set", row); apply->setObjectName("setQuantity_" + name); apply->setAccessibleName("Set " + name);
        if (unit == "state" || unit == "bool") {
            auto *value = new QComboBox(row); value->setObjectName("quantityValue_" + name); value->setAccessibleName(name + " declared value");
            if (unit == "state") { value->addItem("Open", "open"); value->addItem("Closed", "closed"); }
            else { value->addItem("False", false); value->addItem("True", true); }
            const auto current = quantity.value("value").toVariant(); int index = value->findData(current);
            if (index < 0) { value->addItem("Invalid: " + parameterText(quantity.value("value")), current); index = value->count() - 1; }
            value->setCurrentIndex(index); layout->addWidget(value, 1); layout->addWidget(new QLabel(unit, row)); layout->addWidget(apply);
            connect(apply, &QPushButton::clicked, this, [this, componentId, name, unit, value]() {
                if (quantityRefreshing) return;
                auto updated = rawComponent(project, componentId).value("parameters").toObject().value(name).toObject();
                updated["value"] = QJsonValue::fromVariant(value->currentData()); updated["unit"] = unit; project->setParameter(componentId, name, updated);
            });
        } else {
            auto *value = new QLineEdit(parameterText(quantity.value("value")), row); value->setObjectName("quantityValue_" + name); value->setAccessibleName(name + " declared numeric value");
            auto *units = new QComboBox(row); units->setObjectName("quantityUnit_" + name); units->setAccessibleName(name + " unit"); units->setEditable(true);
            units->addItems(ProjectDocument::parameterUnits()); units->setCurrentText(unit);
            layout->addWidget(value, 1); layout->addWidget(units); layout->addWidget(apply);
            const auto commit = [this, componentId, name, value, units]() {
                if (quantityRefreshing) return;
                auto updated = rawComponent(project, componentId).value("parameters").toObject().value(name).toObject();
                bool numeric = false; const double number = value->text().toDouble(&numeric);
                updated["value"] = numeric && qIsFinite(number) ? QJsonValue(number) : QJsonValue(value->text());
                updated["unit"] = units->currentText(); project->setParameter(componentId, name, updated);
            };
            connect(apply, &QPushButton::clicked, this, commit); connect(value, &QLineEdit::returnPressed, this, commit);
            connect(units, &QComboBox::currentTextChanged, this, [this, componentId, name, value](const QString &targetUnit) {
                if (quantityRefreshing) return;
                auto updated = rawComponent(project, componentId).value("parameters").toObject().value(name).toObject();
                bool numeric = false; const double number = value->text().toDouble(&numeric);
                updated["value"] = numeric && qIsFinite(number) ? QJsonValue(number) : QJsonValue(value->text());
                updated = ProjectDocument::quantityForUnit(updated, targetUnit); value->setText(parameterText(updated.value("value")));
                project->setParameter(componentId, name, updated);
            });
        }
        auto *label = new QLabel(QString(name).replace('_', ' '), this); label->setToolTip("Declared parameter; not a measured result."); quantities->addRow(label, row);
    }
    quantityRefreshing = false;
}
void BoardWorkspace::sizePinRows() {
    for (int row = 0; row < pins->rowCount(); ++row) if (auto *control = pins->cellWidget(row, 1)) {
        control->ensurePolished(); pins->setRowHeight(row, qMax(control->sizeHint().height(), qMax(control->minimumSizeHint().height(), control->minimumHeight())) + 2);
    }
}
void BoardWorkspace::changePin(const QString &componentId, const QString &terminalId, int gpio) {
    if (refreshing || gpio < -1) return;
    const auto components = project->components();
    const auto selected = std::find_if(components.begin(), components.end(), [&](const ProjectComponent &component) { return component.id == componentId; });
    if (selected == components.end()) return;
    QString signal;
    for (const auto &terminal : selected->terminals) if (terminal.id == terminalId) signal = terminal.attributes.value("legacy_signal").toString();
    const auto decoder = selected->attributes.value("decoder").toObject();
    const auto kind = decoder.value("kind").toString(), controller = decoder.value("controller").toString();
    const bool shared = (kind == "i2c" && (signal == "sda" || signal == "scl")) || (kind == "spi" && (signal == "mosi" || signal == "miso" || signal == "sclk"));
    QList<QPair<QString, QString>> targets;
    for (const auto &component : components) {
        if (component.id != componentId && (!shared || component.attributes.value("decoder").toObject().value("kind").toString() != kind || component.attributes.value("decoder").toObject().value("controller").toString() != controller)) continue;
        for (const auto &terminal : component.terminals)
            if (terminal.id == terminalId || (shared && terminal.attributes.value("legacy_signal").toString() == signal)) targets.append({component.id, terminal.id});
    }
    const auto pad = gpio >= 0 ? chipTerminal(project, gpio) : QString();
    if (gpio >= 0 && pad.isEmpty()) { message->setText("Add a chip component before connecting a GPIO."); return; }
    project->undoStack()->beginMacro("Connect GPIO branches");
    for (const auto &target : targets) if (auto existing = connectedNet(project, target.second)) project->disconnectTerminal(existing->id, target.second);
    QString netId;
    if (gpio >= 0) {
        const auto existing = connectedNet(project, pad);
        netId = existing ? existing->id : project->addNet({{"name", QString("GPIO %1").arg(gpio)}});
        if (!existing) project->connectTerminal(netId, pad);
        for (const auto &target : targets) project->connectTerminal(netId, target.second);
    }
    for (const auto &target : targets) {
        auto attributes = rawComponent(project, target.first).value("attributes").toObject(); auto cached = attributes.value("decoder").toObject();
        if (!signal.isEmpty() && !cached.isEmpty()) { auto assignments = cached.value("pins").toObject(); assignments[signal] = gpio; cached["pins"] = assignments; attributes["decoder"] = cached; project->setComponentAttributes(target.first, attributes); }
    }
    project->undoStack()->endMacro(); scheduleRefresh();
}
void BoardWorkspace::rebuildCanvas() {
    const bool existing = !scene->items().isEmpty(); const auto transform = canvas->transform(); const auto center = canvas->mapToScene(canvas->viewport()->rect().center());
    refreshing = true; QSignalBlocker blocker(scene); scene->clear(); scene->setBackgroundBrush(ThemeManager::instance()->tokens().background);
    for (int x = 0; x < 1050; x += 24) for (int y = 0; y < 700; y += 24) styleItem(scene->addEllipse(x, y, 1.5, 1.5, Qt::NoPen), Grid);
    const auto components = project->components(); const auto layout = project->geometry().value("components").toObject();
    QMap<QString, DeviceCard *> cards; QMap<QString, QPair<QString, QPointF>> anchors; QMap<QString, int> terminalColors;
    int deviceIndex = 0, chipIndex = 0;
    for (const auto &component : components) {
        const bool chip = component.kind == "mcu"; const int index = chip ? chipIndex++ : deviceIndex++;
        const auto saved = layout.value(component.id).toObject();
        const QPointF location(saved.value("x").toDouble(chip ? 395 + index * 260 : index % 2 ? 760 : 55), saved.value("y").toDouble(chip ? 65 : 55 + (index / 2) * 255));
        const int chipRows = qMax(22, int(component.terminals.size()) - 22);
        auto *card = new DeviceCard(0, 0, chip ? 235 : 210, chip ? 150 + chipRows * 18 : 112 + component.terminals.size() * 22);
        styleItem(card, Layer, StrongBorder); card->setData(ComponentRole, component.id); scene->addItem(card); card->setPos(location);
        card->setFlags(QGraphicsItem::ItemIsMovable | QGraphicsItem::ItemIsSelectable | QGraphicsItem::ItemSendsGeometryChanges);
        card->setToolTip("Drag " + component.name + "; release commits one undo action. Double-click to inspect."); cards[component.id] = card;
        auto label = [card](const QString &value, QPointF point, int role, int size = 11) {
            auto *item = new QGraphicsSimpleTextItem(value, card); item->setFont(QFont("IBM Plex Sans", size)); styleItem(item, role); item->setAcceptedMouseButtons(Qt::NoButton); item->setPos(point);
        };
        label(component.name, {16, 14}, PrimaryText, chip ? 20 : 11); label(component.type, {16, chip ? 55. : 36.}, SecondaryText);
        const auto decoder = component.attributes.value("decoder").toObject(); const int color = busColorRole(decoder.value("kind").toString());
        if (!chip) label(decoder.isEmpty() ? component.kind : decoderLabel(decoder), {16, 60}, color, 9);
        int row = 0;
        for (const auto &terminal : component.terminals) {
            if (chip) {
                const bool left = row < 22; const int local = left ? row : row - 22;
                const QPointF point(left ? 0 : 235, 110 + local * 18);
                label(terminal.gpio ? QString::number(*terminal.gpio) : terminal.name, {left ? 12. : 200., point.y() - 9}, SecondaryText, 9);
                anchors[terminal.id] = {component.id, point};
            } else {
                const int gpio = connectedGpio(project, terminal.id);
                const auto net = connectedNet(project, terminal.id);
                const auto connection = gpio >= 0 ? QString("GPIO %1").arg(gpio) : net ? net->name : QString("NC");
                label(terminal.name + " / " + connection, {16, 86. + row * 22}, SecondaryText);
                anchors[terminal.id] = {component.id, {index % 2 ? 0. : 210., 96. + row * 22}};
            }
            auto *dot = new TerminalDot(anchors[terminal.id].second, card); styleItem(dot, Background, StrongBorder);
            dot->setData(TerminalRole, terminal.id);
            dot->setToolTip(component.name + " / " + terminal.name + "\n" + terminal.id + "\nClick selects. Wire terminals: two clicks. Ctrl: connect to selected net. Alt: disconnect.");
            dot->activated = [this, id = terminal.id](Qt::KeyboardModifiers modifiers) { activateTerminal(id, modifiers); };
            terminalColors[terminal.id] = color; ++row;
        }
        if (chip) label("LOGICAL CHIP TERMINALS", {16, 124. + chipRows * 18}, HelperText, 9);
        card->committed = [this, id = component.id](QPointF position) { if (!refreshing) project->moveComponent(id, position); };
    }
    QList<QPair<ProjectNet, QGraphicsPathItem *>> wires;
    for (const auto &net : project->nets()) {
        int color = Gpio; for (const auto &id : net.endpoints) if (terminalColors.contains(id)) color = terminalColors[id];
        auto *wire = scene->addPath(QPainterPath(), QPen(colorFor(color), 1.6)); styleItem(wire, 0, color); wire->setZValue(-.5); wire->setToolTip("Declared net: " + net.id + " / " + net.name); wires.append({net, wire});
        wire->setData(NetRole, net.id); wire->setAcceptedMouseButtons(Qt::NoButton);
    }
    const auto reroute = [cards, anchors, wires]() {
        for (const auto &entry : wires) {
            QList<QPointF> points;
            for (const auto &id : entry.first.endpoints) if (anchors.contains(id) && cards.contains(anchors[id].first)) points.append(cards[anchors[id].first]->pos() + anchors[id].second);
            QPainterPath path;
            if (points.size() > 1) {
                const auto hub = points.first();
                for (int i = 1; i < points.size(); ++i) { const auto start = points[i]; const qreal middle = (start.x() + hub.x()) / 2; path.moveTo(start); path.cubicTo({middle, start.y()}, {middle, hub.y()}, hub); }
            }
            entry.second->setPath(path);
        }
    };
    for (auto *card : cards) card->moved = [this, reroute](QPointF) { if (!refreshing) reroute(); };
    reroute();
    if (components.isEmpty()) text(scene, "Empty project — add components to begin.", {270, 270}, HelperText, 16);
    scene->setSceneRect(scene->itemsBoundingRect().adjusted(-24, -24, 24, 24)); syncCanvasSelection(); refreshing = false;
    updateNetHighlight();
    if (existing) { canvas->setTransform(transform); canvas->centerOn(center); } else static_cast<CircuitView *>(canvas)->fitCircuit();
}
void BoardWorkspace::updateCanvasTheme() {
    scene->setBackgroundBrush(ThemeManager::instance()->tokens().background); sizePinRows();
    for (auto *item : scene->items()) if (auto *shape = dynamic_cast<QAbstractGraphicsShapeItem *>(item)) {
        const int brush = item->data(BrushRole).toInt(), pen = item->data(PenRole).toInt();
        if (brush) shape->setBrush(colorFor(brush));
        if (pen) { auto value = shape->pen(); value.setColor(colorFor(pen)); shape->setPen(value); }
        shape->update();
    }
    canvas->viewport()->update();
}
void BoardWorkspace::syncCanvasSelection() {
    QSignalBlocker blocker(scene); const auto id = selection->currentData().toString();
    for (auto *item : scene->items()) if (item->data(ComponentRole).isValid()) item->setSelected(item->data(ComponentRole).toString() == id);
}
void BoardWorkspace::addDevice() {
    if (catalogue->currentIndex() >= 7) {
        const QStringList kinds{"voltage-source", "current-source", "resistor", "capacitor", "potentiometer", "switch", "ground", "switch"};
        const auto kind = kinds.value(catalogue->currentIndex() - 7);
        QStringList roles; QJsonObject parameters;
        const auto quantity = [](const QJsonValue &value, const QString &unit) { return QJsonObject{{"value", value}, {"unit", unit}}; };
        if (kind == "voltage-source") { roles = {"p", "n"}; parameters["voltage"] = quantity(3.3, "V"); }
        if (kind == "current-source") { roles = {"p", "n"}; parameters["current"] = quantity(1, "mA"); }
        if (kind == "resistor") { roles = {"a", "b"}; parameters["resistance"] = quantity(1000, "ohm"); }
        if (kind == "capacitor") { roles = {"p", "n"}; parameters["capacitance"] = quantity(10, "uF"); parameters["initial_voltage"] = quantity(0, "V"); }
        if (kind == "potentiometer") { roles = {"a", "w", "b"}; parameters["resistance"] = quantity(10, "kohm"); parameters["position"] = quantity(50, "%"); }
        if (kind == "switch") { roles = {"a", "b"}; parameters["state"] = quantity("open", "state"); parameters["on_resistance"] = quantity(1, "ohm"); parameters["off_resistance"] = quantity(1, "Mohm"); }
        if (kind == "ground") roles = {"ref"};
        QJsonArray terminals;
        for (const auto &role : roles) terminals.append(QJsonObject{{"id", freshId("terminal-")}, {"name", role.toUpper()}, {"role", role},
            {"domain", kind == "ground" ? "ground" : "analog"}, {"direction", "passive"}});
        const auto id = project->addComponent({{"name", catalogue->currentText()}, {"kind", kind},
            {"type", catalogue->currentIndex() == 14 ? QString("button") : kind}, {"terminals", terminals}, {"parameters", parameters}});
        refreshQueued = false; rebuild(); selection->setCurrentIndex(selection->findData(id)); return;
    }
    if (catalogue->currentIndex() == 6) {
        QJsonArray terminals;
        for (int gpio = 0; gpio <= 48; ++gpio) if (validGpio(gpio)) terminals.append(QJsonObject{{"id", freshId("terminal-")}, {"name", QString("GPIO %1").arg(gpio)}, {"role", "gpio"}, {"domain", "digital"}, {"direction", "inout"}, {"gpio", gpio}});
        // New explicit logical-chip rails; migration never adds these implicitly.
        // Native s3-explicit-finite-v1 requires both to be wired to actual rails.
        if (project->profile().value("chip").toString() == "esp32s3") {
            terminals.append(QJsonObject{{"id", freshId("terminal-")}, {"name", "VDD"}, {"role", "vdd"}, {"domain", "power"}, {"direction", "input"}});
            terminals.append(QJsonObject{{"id", freshId("terminal-")}, {"name", "GND"}, {"role", "gnd"}, {"domain", "ground"}, {"direction", "passive"}});
        }
        const auto id = project->addComponent({{"name", "ESP32-S3"}, {"kind", "mcu"}, {"type", "esp32s3"}, {"terminals", terminals}});
        refreshQueued = false; rebuild(); selection->setCurrentIndex(selection->findData(id)); return;
    }
    const QStringList types{"ssd1306", "sht21", "ssd1331", "spi.flash", "uart.loopback", "gpio.test"};
    auto entry = templates.value(types[catalogue->currentIndex()]);
    if (entry.isEmpty()) { message->setText("Open a bundled example to load its component catalogue."); return; }
    const auto id = freshId("component-"); entry["id"] = id; entry["name"] = entry.value("type").toString(); entry.remove("legacy");
    auto attributes = entry.value("attributes").toObject(); auto decoder = attributes.value("decoder").toObject();
    const auto kind = decoder.value("kind").toString(), controller = decoder.value("controller").toString();
    if (kind == "i2c") {
        QSet<int> used; for (const auto &component : project->components()) { const auto bus = component.attributes.value("decoder").toObject(); if (bus.value("controller").toString() == controller) used.insert(bus.value("address").toString().toInt(nullptr, 0)); }
        int address = decoder.value("address").toString().toInt(nullptr, 0); while (used.contains(address) && address <= 0x77) ++address;
        if (address > 0x77) { message->setText("No free I2C address on this cached controller."); return; }
        const auto text = QString("0x%1").arg(address, 2, 16, QLatin1Char('0')); decoder["address"] = text;
        auto simulator = entry.value("simulator").toObject(); auto args = simulator.value("args").toArray();
        for (int i = 0; i + 1 < args.size(); ++i) if (args[i] == "--address") args[i + 1] = text;
        simulator["args"] = args; entry["simulator"] = simulator;
    }
    auto terminals = entry.value("terminals").toArray(); QList<QPair<QString, QString>> connections;
    auto assignments = decoder.value("pins").toObject();
    for (int i = 0; i < terminals.size(); ++i) {
        auto terminal = terminals[i].toObject(); const auto terminalId = freshId("terminal-"); terminal["id"] = terminalId; terminals[i] = terminal;
        const auto signal = terminal.value("attributes").toObject().value("legacy_signal").toString();
        const bool shared = kind == "i2c" || (kind == "spi" && (signal == "mosi" || signal == "miso" || signal == "sclk"));
        assignments[signal] = -1;
        if (shared) for (const auto &component : project->components()) {
            const auto bus = component.attributes.value("decoder").toObject(); if (bus.value("kind").toString() != kind || bus.value("controller").toString() != controller) continue;
            for (const auto &existing : component.terminals) if (existing.attributes.value("legacy_signal").toString() == signal) {
                const auto net = connectedNet(project, existing.id); if (net) { connections.append({terminalId, net->id}); assignments[signal] = connectedGpio(project, existing.id); } break;
            }
            if (!connections.isEmpty() && connections.last().first == terminalId) break;
        }
    }
    decoder["pins"] = assignments; attributes["decoder"] = decoder; entry["attributes"] = attributes; entry["terminals"] = terminals;
    project->undoStack()->beginMacro("Add component");
    project->addComponent(entry);
    for (const auto &connection : connections) project->connectTerminal(connection.second, connection.first);
    project->undoStack()->endMacro(); refreshQueued = false; rebuild(); selection->setCurrentIndex(selection->findData(id));
}
void BoardWorkspace::removeDevice() {
    const auto id = selection->currentData().toString(); if (id.isEmpty()) return; project->removeComponent(id);
}
