#include "SchemaDevicePanel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QPushButton>
#include <QSpinBox>
#include <QTextEdit>
#include <QVBoxLayout>

static void clearLayout(QLayout *layout)
{
    if (!layout) {
        return;
    }
    while (QLayoutItem *item = layout->takeAt(0)) {
        if (item->layout()) {
            clearLayout(item->layout());
            delete item->layout();
        }
        if (item->widget()) {
            item->widget()->deleteLater();
        }
        delete item;
    }
}

SchemaDevicePanel::SchemaDevicePanel(const QString &deviceId,
                                     const QString &deviceType,
                                     const QJsonObject &rawConfig,
                                     QWidget *parent)
    : DevicePanelBase(deviceId, deviceType, rawConfig, parent)
{
    buildBaseUi();

    // Coalesce rapid display updates — fires at most every 50ms
    m_renderTimer.setSingleShot(true);
    m_renderTimer.setInterval(50);
    connect(&m_renderTimer, &QTimer::timeout, this, &SchemaDevicePanel::doCoalescedRender);
}

void SchemaDevicePanel::buildBaseUi()
{
    auto *layout = contentLayout();

    auto *controlsGroup = new QGroupBox("Device Controls", this);
    auto *controlsVBox = new QVBoxLayout(controlsGroup);

    m_panelTitleLabel = new QLabel(this);
    m_panelTitleLabel->setStyleSheet("font-weight: 600;");
    m_panelTitleLabel->hide();
    controlsVBox->addWidget(m_panelTitleLabel);

    m_panelDescriptionLabel = new QLabel(this);
    m_panelDescriptionLabel->setWordWrap(true);
    m_panelDescriptionLabel->setStyleSheet("color: #888; font-size: 11px;");
    m_panelDescriptionLabel->hide();
    controlsVBox->addWidget(m_panelDescriptionLabel);

    m_controlsHost = new QWidget(this);
    m_controlsRootLayout = new QVBoxLayout(m_controlsHost);
    m_controlsRootLayout->setContentsMargins(0, 0, 0, 0);
    m_controlsRootLayout->setSpacing(8);
    controlsVBox->addWidget(m_controlsHost);

    layout->addWidget(controlsGroup);

    auto *stateGroup = new QGroupBox("Live State", this);
    auto *stateLayout = new QVBoxLayout(stateGroup);
    m_stateView = new QTextEdit(this);
    m_stateView->setReadOnly(true);
    m_stateView->setFont(QFont("Courier New", 10));
    m_stateView->setStyleSheet(
        "background: #0a0a0a; color: #c0c0c0; border: 1px solid #333;");
    m_stateView->setPlaceholderText("Waiting for device state...");
    stateLayout->addWidget(m_stateView);
    layout->addWidget(stateGroup, 1);
}

QVariant SchemaDevicePanel::valueFromPath(const QJsonObject &state, const QString &path) const
{
    if (path.trimmed().isEmpty()) {
        return QVariant();
    }

    QStringList tokens = path.split('.', Qt::SkipEmptyParts);
    if (tokens.isEmpty()) {
        return QVariant();
    }

    QJsonValue current(state.value(tokens.first()));
    for (int index = 1; index < tokens.size(); ++index) {
        if (!current.isObject()) {
            return QVariant();
        }
        current = current.toObject().value(tokens.at(index));
    }

    if (!current.isUndefined()) {
        return current.toVariant();
    }

    const QJsonObject telemetry = state.value("telemetry").toObject();
    current = telemetry.value(tokens.first());
    for (int index = 1; index < tokens.size(); ++index) {
        if (!current.isObject()) {
            return QVariant();
        }
        current = current.toObject().value(tokens.at(index));
    }
    return current.isUndefined() ? QVariant() : current.toVariant();
}

QString SchemaDevicePanel::formatMetricValue(const QVariant &value,
                                             const MetricBinding &binding) const
{
    if (!value.isValid()) {
        return QStringLiteral("-");
    }

    QString text;
    if (value.typeId() == QMetaType::Bool) {
        const bool enabled = value.toBool();
        const QString trueText = binding.boolTrueText.trimmed().isEmpty()
            ? QStringLiteral("ON")
            : binding.boolTrueText;
        const QString falseText = binding.boolFalseText.trimmed().isEmpty()
            ? QStringLiteral("OFF")
            : binding.boolFalseText;
        text = enabled ? trueText : falseText;
    } else if (binding.decimals >= 0 && value.canConvert<double>()) {
        text = QString::number(value.toDouble(), 'f', binding.decimals);
    } else {
        text = value.toString();
    }

    if (!binding.unit.trimmed().isEmpty()) {
        text += QString(" %1").arg(binding.unit.trimmed());
    }
    return text;
}

void SchemaDevicePanel::rebuildDisplayView(const QJsonObject &panelObj)
{
    const QString kind = panelObj.value("kind").toString().trimmed().toLower();
    const QJsonObject displayObj = panelObj.value("display").toObject();
    if (kind != "display" && displayObj.isEmpty()) {
        return;
    }

    m_displayWidth = panelObj.value("width").toInt(128);
    m_displayHeight = panelObj.value("height").toInt(64);
    m_framePrimaryKey = displayObj.value("state_key").toString("frame_update").trimmed();
    m_frameFallbackKey = displayObj.value("fallback_state_key").toString("buffer").trimmed();
    m_frameLayout = displayObj.value("layout").toString("page-major").trimmed().toLower();
    m_frameEncoding = displayObj.value("encoding").toString("u8").trimmed().toLower();

    auto *group = new QGroupBox("Screen Output", this);
    auto *box = new QVBoxLayout(group);

    m_displayHost = group;
    m_displayLabel = new QLabel(this);
    m_displayLabel->setAlignment(Qt::AlignCenter);
    m_displayLabel->setStyleSheet("background:#000; border:1px solid #333; padding:4px;");

    m_displayZoom = new QSpinBox(this);
    m_displayZoom->setRange(1, 8);
    m_displayZoom->setValue(3);
    m_displayZoom->setSuffix("x");

    auto *zoomLine = new QHBoxLayout();
    zoomLine->addWidget(new QLabel("Zoom:", this));
    zoomLine->addWidget(m_displayZoom);
    zoomLine->addStretch(1);
    box->addLayout(zoomLine);

    m_displayImage = QImage(m_displayWidth, m_displayHeight, QImage::Format_RGB32);
    m_displayImage.fill(QColor(0, 0, 0));
    m_displayLabel->setPixmap(QPixmap::fromImage(scaleForDisplay(m_displayImage)));
    box->addWidget(m_displayLabel);

    m_displayMetaLabel = new QLabel(
        QString("Resolution: %1 x %2  |  Format: %3")
            .arg(m_displayWidth)
            .arg(m_displayHeight)
            .arg(panelObj.value("pixel_format").toString("mono1")),
        this);
    m_displayMetaLabel->setStyleSheet("color:#888; font-size: 10px;");
    box->addWidget(m_displayMetaLabel);

    connect(m_displayZoom, qOverload<int>(&QSpinBox::valueChanged), this, [this]() {
        if (m_displayLabel && !m_displayImage.isNull()) {
            m_displayLabel->setPixmap(QPixmap::fromImage(scaleForDisplay(m_displayImage)));
        }
    });

    m_controlsRootLayout->addWidget(group);
}

void SchemaDevicePanel::updateDisplayView(const QJsonObject &state)
{
    if (!m_displayLabel) {
        return;
    }

    bool visualStateChanged = false;
    if (state.contains("display_on")) {
        const bool next = state.value("display_on").toBool(m_displayOn);
        visualStateChanged = visualStateChanged || (next != m_displayOn);
        m_displayOn = next;
    }
    if (state.contains("inverted")) {
        const bool next = state.value("inverted").toBool(m_displayInverted);
        visualStateChanged = visualStateChanged || (next != m_displayInverted);
        m_displayInverted = next;
    }
    if (state.contains("entire_display_on")) {
        const bool next = state.value("entire_display_on").toBool(m_displayEntireOn);
        visualStateChanged = visualStateChanged || (next != m_displayEntireOn);
        m_displayEntireOn = next;
    }
    if (state.contains("contrast")) {
        const int next = qBound(0, state.value("contrast").toInt(m_displayContrast), 255);
        visualStateChanged = visualStateChanged || (next != m_displayContrast);
        m_displayContrast = next;
    }

    if (state.contains("geometry")) {
        const QJsonObject geo = state.value("geometry").toObject();
        m_displayWidth = geo.value("width").toInt(m_displayWidth);
        m_displayHeight = geo.value("height").toInt(m_displayHeight);
        if (m_displayMetaLabel) {
            m_displayMetaLabel->setText(
                QString("Resolution: %1 x %2  |  Format: mono1")
                    .arg(m_displayWidth)
                    .arg(m_displayHeight));
        }
    }

    const QVariant primaryBuffer = valueFromPath(state, m_framePrimaryKey);
    const QVariant fallbackBuffer = valueFromPath(state, m_frameFallbackKey);

    if (primaryBuffer.isValid() && primaryBuffer.canConvert<QVariantMap>()) {
        m_lastDisplayBuffer = QJsonObject::fromVariantMap(primaryBuffer.toMap());
        scheduleRender();
    } else if (fallbackBuffer.isValid() && fallbackBuffer.canConvert<QVariantMap>()) {
        m_lastDisplayBuffer = QJsonObject::fromVariantMap(fallbackBuffer.toMap());
        scheduleRender();
    } else if (visualStateChanged && !m_lastDisplayBuffer.isEmpty()) {
        scheduleRender();
    }
}

void SchemaDevicePanel::scheduleRender()
{
    m_renderPending = true;
    if (!m_renderTimer.isActive()) {
        m_renderTimer.start();
    }
}

void SchemaDevicePanel::doCoalescedRender()
{
    if (!m_renderPending || m_lastDisplayBuffer.isEmpty()) {
        m_renderPending = false;
        return;
    }
    m_renderPending = false;
    renderDisplayBuffer(m_lastDisplayBuffer);
}

void SchemaDevicePanel::renderDisplayBuffer(const QJsonObject &bufferObj)
{
    if (bufferObj.isEmpty()) {
        return;
    }

    m_lastDisplayBuffer = bufferObj;

    const QString encoding = bufferObj.value("encoding").toString(m_frameEncoding).trimmed().toLower();
    const QString layout = bufferObj.value("layout").toString(m_frameLayout).trimmed().toLower();
    if (encoding != "u8" || layout != "page-major") {
        return;
    }

    const QJsonArray dataArr = bufferObj.value("data").toArray();
    if (dataArr.isEmpty()) {
        return;
    }

    QList<int> data;
    data.reserve(dataArr.size());
    for (const QJsonValue &value : dataArr) {
        data.append(value.toInt());
    }
    renderPageMajorMono(data, m_displayWidth, m_displayHeight);
}

void SchemaDevicePanel::renderPageMajorMono(const QList<int> &data, int w, int h)
{
    if (w <= 0 || h <= 0) {
        return;
    }

    QImage img(w, h, QImage::Format_RGB32);
    img.fill(QColor(0, 0, 0));

    const int pages = qMax(1, h / 8);
    const int bright = qBound(40, m_displayContrast, 255);

    // Pre-compute ARGB pixel values for fast scanline writes
    const QRgb onRgb = m_displayInverted
        ? qRgb(0, 0, 0)
        : qRgb(bright, bright, qBound(0, bright + 30, 255));
    const QRgb offRgb = m_displayInverted
        ? qRgb(bright, bright, bright)
        : qRgb(0, 0, 0);

    for (int page = 0; page < pages && page * w < data.size(); ++page) {
        for (int col = 0; col < w && page * w + col < data.size(); ++col) {
            const int byte = data.at(page * w + col);
            for (int bit = 0; bit < 8; ++bit) {
                const int y = page * 8 + bit;
                if (y >= h) {
                    break;
                }
                const bool pixelOn = ((byte >> bit) & 0x01) != 0;
                // Direct scanline write — ~10x faster than setPixelColor()
                reinterpret_cast<QRgb *>(img.scanLine(y))[col] = pixelOn ? onRgb : offRgb;
            }
        }
    }

    if (!m_displayOn) {
        img.fill(QColor(5, 5, 8));
    } else if (m_displayEntireOn) {
        img.fill(QColor(qRed(onRgb), qGreen(onRgb), qBlue(onRgb)));
    }

    m_displayImage = img;
    if (m_displayLabel) {
        m_displayLabel->setPixmap(QPixmap::fromImage(scaleForDisplay(img)));
    }
}

QImage SchemaDevicePanel::scaleForDisplay(const QImage &source) const
{
    const int scale = m_displayZoom ? m_displayZoom->value() : 3;
    return source.scaled(source.width() * scale,
                         source.height() * scale,
                         Qt::KeepAspectRatio,
                         Qt::FastTransformation);
}

void SchemaDevicePanel::rebuildMetricsView(const QJsonObject &panelObj)
{
    const QJsonArray metrics = panelObj.value("metrics").toArray();
    if (metrics.isEmpty()) {
        return;
    }

    auto *group = new QGroupBox("Live Metrics", this);
    auto *grid = new QGridLayout(group);

    int row = 0;
    int col = 0;
    for (const QJsonValue &value : metrics) {
        const QJsonObject metric = value.toObject();
        const QString path = metric.value("state_path").toString().trimmed();
        if (path.isEmpty()) {
            continue;
        }

        const QString labelText = metric.value("label").toString(path);
        auto *card = new QGroupBox(labelText, this);
        auto *cardLayout = new QVBoxLayout(card);

        auto *valLabel = new QLabel("-", this);
        valLabel->setStyleSheet("font-size: 18px; font-weight: 600;");
        cardLayout->addWidget(valLabel);

        grid->addWidget(card, row, col);

        MetricBinding binding;
        binding.path = path;
        binding.decimals = metric.value("decimals").toInt(-1);
        binding.unit = metric.value("unit").toString();
        binding.boolTrueText = metric.value("true_text").toString();
        binding.boolFalseText = metric.value("false_text").toString();
        binding.valueLabel = valLabel;
        m_metrics.append(binding);

        col++;
        if (col >= 3) {
            col = 0;
            row++;
        }
    }

    m_controlsRootLayout->addWidget(group);
}

void SchemaDevicePanel::updateMetricsView(const QJsonObject &state)
{
    for (const MetricBinding &binding : m_metrics) {
        if (!binding.valueLabel) {
            continue;
        }
        const QVariant value = valueFromPath(state, binding.path);
        binding.valueLabel->setText(formatMetricValue(value, binding));
    }
}

void SchemaDevicePanel::rebuildScriptsView(const QJsonObject &panelObj)
{
    const QJsonArray scripts = panelObj.value("scripts").toArray();
    if (scripts.isEmpty()) {
        return;
    }

    auto *group = new QGroupBox("Scripts", this);
    auto *layout = new QVBoxLayout(group);

    for (const QJsonValue &value : scripts) {
        const QJsonObject scriptObj = value.toObject();
        const QString path = scriptObj.value("state_path").toString().trimmed();
        if (path.isEmpty()) {
            continue;
        }

        const QString title = scriptObj.value("title").toString(path);
        layout->addWidget(new QLabel(title, this));

        auto *view = new QTextEdit(this);
        view->setReadOnly(true);
        view->setMinimumHeight(scriptObj.value("min_height").toInt(80));
        view->setPlaceholderText("No script content yet");
        layout->addWidget(view);

        ScriptBinding binding;
        binding.path = path;
        binding.view = view;
        m_scriptViews.append(binding);
    }

    m_controlsRootLayout->addWidget(group);
}

void SchemaDevicePanel::updateScriptsView(const QJsonObject &state)
{
    for (const ScriptBinding &binding : m_scriptViews) {
        if (!binding.view) {
            continue;
        }
        const QVariant value = valueFromPath(state, binding.path);
        if (!value.isValid()) {
            continue;
        }

        if (value.typeId() == QMetaType::QVariantMap ||
            value.typeId() == QMetaType::QVariantList) {
            const QJsonValue jsonValue = QJsonValue::fromVariant(value);
            if (jsonValue.isObject()) {
                binding.view->setPlainText(QString::fromUtf8(
                    QJsonDocument(jsonValue.toObject()).toJson(QJsonDocument::Indented)));
            } else if (jsonValue.isArray()) {
                binding.view->setPlainText(QString::fromUtf8(
                    QJsonDocument(jsonValue.toArray()).toJson(QJsonDocument::Indented)));
            }
        } else {
            binding.view->setPlainText(value.toString());
        }
    }
}

void SchemaDevicePanel::emitControlChange(const QJsonObject &ctrlObj,
                                          const QString &name,
                                          const QVariant &value,
                                          const QString &eventType)
{
    if (!m_scriptOwnedPanel || m_scriptEventMethod.trimmed().isEmpty()) {
        emit parameterChangeRequested(deviceId(), name, value);
        return;
    }

    QJsonObject params;
    params["event"] = eventType;
    params["control"] = name;
    params["value"] = QJsonValue::fromVariant(value);
    if (ctrlObj.contains("rpc_method")) {
        params["rpc_method"] = ctrlObj.value("rpc_method").toString();
    }
    if (ctrlObj.contains("rpc_params")) {
        params["rpc_params"] = ctrlObj.value("rpc_params").toObject();
    }

    emit parameterChangeRequested(deviceId(),
                                 QString("__rpc:%1").arg(m_scriptEventMethod),
                                 QVariant::fromValue(params));

    if (m_scriptFallbackToSetParameter && eventType == "set_control") {
        emit parameterChangeRequested(deviceId(), name, value);
    }
}

QWidget *SchemaDevicePanel::buildControlWidget(const QJsonObject &ctrlObj,
                                               const QString &name,
                                               const QString &type)
{
    const bool writable = ctrlObj.value("writable").toBool(type == "action");
    const QString helpText = ctrlObj.value("description").toString().trimmed();

    if (type == "bool") {
        auto *check = new QCheckBox(this);
        check->setEnabled(writable);
        if (!helpText.isEmpty()) {
            check->setToolTip(helpText);
        }
        connect(check, &QCheckBox::toggled, this, [this, name, ctrlObj](bool v) {
            emitControlChange(ctrlObj, name, v);
        });
        return check;
    }

    if (type == "int") {
        auto *spin = new QSpinBox(this);
        spin->setRange(ctrlObj.value("min").toInt(-2147483647),
                       ctrlObj.value("max").toInt(2147483647));
        spin->setSingleStep(ctrlObj.value("step").toInt(1));
        spin->setEnabled(writable);
        if (!helpText.isEmpty()) {
            spin->setToolTip(helpText);
        }
        connect(spin, &QSpinBox::editingFinished, this, [this, name, spin, ctrlObj]() {
            emitControlChange(ctrlObj, name, spin->value());
        });
        return spin;
    }

    if (type == "float" || type == "double") {
        auto *spin = new QDoubleSpinBox(this);
        spin->setRange(ctrlObj.value("min").toDouble(-1e12),
                       ctrlObj.value("max").toDouble(1e12));
        spin->setSingleStep(ctrlObj.value("step").toDouble(0.1));
        spin->setDecimals(ctrlObj.value("decimals").toInt(3));
        spin->setEnabled(writable);
        const QString unit = ctrlObj.value("unit").toString().trimmed();
        if (!unit.isEmpty()) {
            spin->setSuffix(QString(" %1").arg(unit));
        }
        if (!helpText.isEmpty()) {
            spin->setToolTip(helpText);
        }
        connect(spin, &QDoubleSpinBox::editingFinished, this, [this, name, spin, ctrlObj]() {
            emitControlChange(ctrlObj, name, spin->value());
        });
        return spin;
    }

    if (type == "enum") {
        auto *combo = new QComboBox(this);
        const QJsonArray options = ctrlObj.value("enum").toArray();
        for (const QJsonValue &option : options) {
            if (option.isDouble()) {
                combo->addItem(QString::number(option.toDouble()), option.toVariant());
            } else {
                const QString text = option.toVariant().toString();
                combo->addItem(text, option.toVariant());
            }
        }
        combo->setEnabled(writable);
        if (!helpText.isEmpty()) {
            combo->setToolTip(helpText);
        }
        connect(combo, &QComboBox::currentIndexChanged, this,
                [this, name, combo, ctrlObj](int idx) {
                    if (idx < 0) return;
                    const QVariant data = combo->itemData(idx);
                    emitControlChange(ctrlObj, name,
                                      data.isValid() ? data : combo->currentText());
                });
        return combo;
    }

    if (type == "action") {
        const QString text = ctrlObj.value("label").toString().trimmed().isEmpty()
            ? name
            : ctrlObj.value("label").toString().trimmed();
        auto *btn = new QPushButton(text, this);
        btn->setEnabled(writable);
        if (!helpText.isEmpty()) {
            btn->setToolTip(helpText);
        }
        connect(btn, &QPushButton::clicked, this, [this, name, ctrlObj]() {
            const QString rpcMethod = ctrlObj.value("rpc_method").toString().trimmed();
            const QJsonObject rpcParams = ctrlObj.value("rpc_params").toObject();
            if (m_scriptOwnedPanel && !m_scriptEventMethod.trimmed().isEmpty()) {
                emitControlChange(ctrlObj, name, true, "action");
                return;
            }
            if (!rpcMethod.isEmpty()) {
                emit parameterChangeRequested(deviceId(),
                                             QString("__rpc:%1").arg(rpcMethod),
                                             QVariant::fromValue(rpcParams));
                return;
            }
            emit parameterChangeRequested(deviceId(), name, true);
        });
        return btn;
    }

    auto *line = new QLineEdit(this);
    line->setEnabled(writable);
    const QString placeholder = ctrlObj.value("placeholder").toString().trimmed();
    if (!placeholder.isEmpty()) {
        line->setPlaceholderText(placeholder);
    }
    if (!helpText.isEmpty()) {
        line->setToolTip(helpText);
    }
    connect(line, &QLineEdit::editingFinished, this, [this, name, line, ctrlObj]() {
        emitControlChange(ctrlObj, name, line->text());
    });
    return line;
}

QFormLayout *SchemaDevicePanel::ensureSectionLayout(const QString &sectionName)
{
    const QString normalized = sectionName.trimmed().isEmpty()
        ? QStringLiteral("General")
        : sectionName.trimmed();

    if (m_sectionLayouts.contains(normalized)) {
        return m_sectionLayouts.value(normalized);
    }

    auto *sectionGroup = new QGroupBox(normalized, this);
    auto *form = new QFormLayout(sectionGroup);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_controlsRootLayout->addWidget(sectionGroup);

    m_sectionLayouts.insert(normalized, form);
    return form;
}

void SchemaDevicePanel::rebuildControls(const QJsonObject &panelObj)
{
    clearLayout(m_controlsRootLayout);
    m_bindings.clear();
    m_sectionLayouts.clear();
    m_metrics.clear();
    m_scriptViews.clear();
    m_displayHost = nullptr;
    m_displayLabel = nullptr;
    m_displayMetaLabel = nullptr;
    m_displayZoom = nullptr;
    m_scriptOwnedPanel = false;
    m_scriptEventMethod = "panel_event";
    m_scriptStateMethod.clear();
    m_scriptRuntimeStateKey = "panel_runtime";
    m_scriptFallbackToSetParameter = true;
    m_lastScriptStatePayload.clear();
    m_framePrimaryKey = "frame_update";
    m_frameFallbackKey = "buffer";
    m_frameLayout = "page-major";
    m_frameEncoding = "u8";

    const QString title = panelObj.value("title").toString().trimmed();
    if (!title.isEmpty()) {
        m_panelTitleLabel->setText(title);
        m_panelTitleLabel->show();
    } else {
        m_panelTitleLabel->hide();
    }

    const QString desc = panelObj.value("description").toString().trimmed();
    if (!desc.isEmpty()) {
        m_panelDescriptionLabel->setText(desc);
        m_panelDescriptionLabel->show();
    } else {
        m_panelDescriptionLabel->hide();
    }

    const QJsonArray controls = panelObj.value("controls").toArray();

    const QJsonObject scriptObj = panelObj.value("script").toObject();
    m_scriptOwnedPanel = scriptObj.value("enabled").toBool(false);
    m_scriptEventMethod = scriptObj.value("event_method").toString("panel_event").trimmed();
    m_scriptStateMethod = scriptObj.value("state_method").toString().trimmed();
    m_scriptRuntimeStateKey = scriptObj.value("runtime_state_key").toString("panel_runtime").trimmed();
    m_scriptFallbackToSetParameter = scriptObj.value("fallback_set_parameter").toBool(true);

    if (m_scriptOwnedPanel && !m_scriptRuntimeStateKey.isEmpty()) {
        if (!m_framePrimaryKey.contains('.')) {
            m_framePrimaryKey = QString("%1.%2").arg(m_scriptRuntimeStateKey, m_framePrimaryKey);
        }
        if (!m_frameFallbackKey.contains('.')) {
            m_frameFallbackKey = QString("%1.%2").arg(m_scriptRuntimeStateKey, m_frameFallbackKey);
        }
    }

    rebuildDisplayView(panelObj);
    rebuildMetricsView(panelObj);
    rebuildScriptsView(panelObj);

    if (controls.isEmpty()) {
        m_controlsRootLayout->addWidget(new QLabel("No controls exposed by device", this));
        m_controlsRootLayout->addStretch(1);
        return;
    }

    for (const QJsonValue &v : controls) {
        const QJsonObject ctrl = v.toObject();
        const QString name = ctrl.value("name").toString().trimmed();
        if (name.isEmpty()) {
            continue;
        }
        const QString type = ctrl.value("type").toString("string").trimmed().toLower();
        const QString labelText = ctrl.value("label").toString().trimmed().isEmpty()
            ? name
            : ctrl.value("label").toString().trimmed();
        const QString section = ctrl.value("section").toString();

        QWidget *widget = buildControlWidget(ctrl, name, type);

        auto *form = ensureSectionLayout(section);
        auto *label = new QLabel(labelText, this);
        const QString helpText = ctrl.value("description").toString().trimmed();
        if (!helpText.isEmpty()) {
            label->setToolTip(helpText);
        }
        form->addRow(label, widget);
        m_bindings.insert(name, ControlBinding{type, widget});
    }

    m_controlsRootLayout->addStretch(1);
}

QVariant SchemaDevicePanel::valueFromState(const QJsonObject &state, const QString &key) const
{
    if (state.contains(key)) {
        return state.value(key).toVariant();
    }

    const QJsonObject telemetry = state.value("telemetry").toObject();
    if (telemetry.contains(key)) {
        return telemetry.value(key).toVariant();
    }

    return QVariant();
}

void SchemaDevicePanel::updateCapabilities(const QJsonObject &caps)
{
    const QJsonObject panelObj = caps.value("panel").toObject();
    if (panelObj.isEmpty()) {
        return;
    }

    if (panelObj == m_lastPanel) {
        return;
    }
    m_lastPanel = panelObj;
    rebuildControls(panelObj);
}

void SchemaDevicePanel::updateState(const QJsonObject &state)
{
    // Only update the expensive JSON text view when state actually changes.
    const QByteArray stateCompact = QJsonDocument(state).toJson(QJsonDocument::Compact);
    if (stateCompact != m_lastStateHash) {
        m_lastStateHash = stateCompact;
        m_stateView->setPlainText(
            QString::fromUtf8(QJsonDocument(state).toJson(QJsonDocument::Indented)));
    }

    updateDisplayView(state);
    updateMetricsView(state);
    updateScriptsView(state);

    if (m_scriptOwnedPanel && !m_scriptStateMethod.trimmed().isEmpty()) {
        QJsonObject params;
        params["state"] = state;
        params["panel"] = m_lastPanel;
        const QByteArray serialized = QJsonDocument(params).toJson(QJsonDocument::Compact);
        if (serialized != m_lastScriptStatePayload) {
            m_lastScriptStatePayload = serialized;
            emit parameterChangeRequested(deviceId(),
                                         QString("__rpc:%1").arg(m_scriptStateMethod),
                                         QVariant::fromValue(params));
        }
    }

    for (auto it = m_bindings.begin(); it != m_bindings.end(); ++it) {
        const QVariant value = valueFromState(state, it.key());
        if (!value.isValid()) {
            continue;
        }

        QWidget *widget = it.value().widget;
        const QString type = it.value().type;

        if (auto *check = qobject_cast<QCheckBox *>(widget)) {
            check->blockSignals(true);
            check->setChecked(value.toBool());
            check->blockSignals(false);
        } else if (auto *spin = qobject_cast<QSpinBox *>(widget)) {
            spin->blockSignals(true);
            spin->setValue(value.toInt());
            spin->blockSignals(false);
        } else if (auto *dspin = qobject_cast<QDoubleSpinBox *>(widget)) {
            dspin->blockSignals(true);
            dspin->setValue(value.toDouble());
            dspin->blockSignals(false);
        } else if (auto *combo = qobject_cast<QComboBox *>(widget)) {
            const QVariant target = value;
            int idx = combo->findData(target);
            if (idx < 0) {
                idx = combo->findText(target.toString());
            }
            if (idx >= 0) {
                combo->blockSignals(true);
                combo->setCurrentIndex(idx);
                combo->blockSignals(false);
            }
        } else if (auto *line = qobject_cast<QLineEdit *>(widget)) {
            if (type != "action") {
                line->blockSignals(true);
                line->setText(value.toString());
                line->blockSignals(false);
            }
        }
    }
}
