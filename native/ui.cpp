#include "accessibility.hpp"
#include "actions.hpp"
#include "autostart.hpp"
#include "capture.hpp"
#include "design.hpp"
#include "gnome.hpp"
#include "i18n.hpp"
#include "portal.hpp"
#include "reader.hpp"
#include "shortcut_recorder.hpp"
#include "x11.hpp"
#ifdef Q_OS_MACOS
#include "macos.hpp"
#endif
#include <QImageReader>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QTemporaryDir>
#include <QtWidgets>
#include <cmath>
#include <limits>
#include <memory>
#ifdef HAVE_KGLOBALACCEL
#include <KGlobalAccel>
#include <KGlobalShortcutInfo>
#endif

using Callback = void (*)(const char *);
static Callback callback = nullptr;
static void send(QJsonObject value) {
    auto bytes = QJsonDocument(value).toJson(QJsonDocument::Compact);
    if (callback)
        callback(bytes.constData());
}
static void action(const QString &id) { send({{"type", "action"}, {"action", id}}); }
static QLabel *label(const QString &text, const QString &name = {}) {
    auto *l = new QLabel(text);
    l->setObjectName(name);
    l->setTextFormat(Qt::PlainText);
    return l;
}
static QPushButton *button(const QString &text, const QString &name = {}) {
    auto *b = new GlassButton(text);
    b->setObjectName(name);
    b->setCursor(Qt::PointingHandCursor);
    return b;
}
static QPushButton *iconButton(const QString &glyph, const QString &name) {
    auto *b = button("");
    b->setProperty("glyph", glyph);
    b->setProperty("quiet", true);
    b->setFixedSize(34, 34);
    b->setAccessibleName(name);
    b->setToolTip(name);
    return b;
}
#ifdef HAVE_KGLOBALACCEL
static bool shortcutConflict(const QKeySequence &key) {
    const auto owners = KGlobalAccel::globalShortcutsByKey(key);
    for (const auto &owner : owners)
        if (owner.componentUniqueName() != "ranslator")
            return true;
    return false;
}
#endif
static bool modifierOnly(const QString &sequence) {
    for (const auto &part : sequence.split('+'))
        if (!QStringList{"Ctrl", "Control", "Alt", "Shift", "Meta", "Super"}.contains(
                part.trimmed(), Qt::CaseInsensitive))
            return false;
    return !sequence.trimmed().isEmpty();
}
static QComboBox *languages(bool source) {
    auto *box = new GlassComboBox;
    box->setEditable(true);
    box->setMinimumWidth(170);
    box->addItem(source ? UiText::tr("Detect language") : UiText::tr("Automatic target"), "auto");
    const QList<QPair<QString, QString>> list = {{"English", "en"},
                                                 {"Español", "es"},
                                                 {"Русский", "ru"},
                                                 {"Deutsch", "de"},
                                                 {"Français", "fr"},
                                                 {"Italiano", "it"},
                                                 {"Português", "pt"},
                                                 {"Українська", "uk"},
                                                 {"Polski", "pl"},
                                                 {"中文", "zh"},
                                                 {"日本語", "ja"},
                                                 {"한국어", "ko"},
                                                 {"العربية", "ar"},
                                                 {"हिन्दी", "hi"},
                                                 {"Türkçe", "tr"},
                                                 {"Nederlands", "nl"},
                                                 {"Svenska", "sv"},
                                                 {"Dansk", "da"},
                                                 {"Norsk", "nb"},
                                                 {"Suomi", "fi"},
                                                 {"Čeština", "cs"},
                                                 {"Slovenčina", "sk"},
                                                 {"Română", "ro"},
                                                 {"Magyar", "hu"},
                                                 {"Ελληνικά", "el"},
                                                 {"עברית", "he"},
                                                 {"فارسی", "fa"},
                                                 {"বাংলা", "bn"},
                                                 {"Tiếng Việt", "vi"},
                                                 {"ไทย", "th"},
                                                 {"Bahasa Indonesia", "id"},
                                                 {"Bahasa Melayu", "ms"},
                                                 {"Català", "ca"},
                                                 {"Български", "bg"},
                                                 {"Српски", "sr"},
                                                 {"Hrvatski", "hr"},
                                                 {"Eesti", "et"},
                                                 {"Latviešu", "lv"},
                                                 {"Lietuvių", "lt"}};
    for (const auto &item : list)
        box->addItem(item.first, item.second);
    box->setInsertPolicy(QComboBox::NoInsert);
    box->setToolTip(UiText::tr(
        "Choose a language, or type any ISO code / language name supported by your provider."));
    box->setAccessibleName(source ? UiText::tr("Source language")
                                  : UiText::tr("Translation language"));
    return box;
}
static QString languageValue(QComboBox *box) {
    return box->currentIndex() >= 0 && box->currentText() == box->itemText(box->currentIndex())
               ? box->currentData().toString()
               : box->currentText().trimmed();
}
static void setLanguage(QComboBox *box, const QString &value) {
    int index = box->findData(value);
    if (index >= 0)
        box->setCurrentIndex(index);
    else
        box->setEditText(value);
}
static void compactLanguage(QComboBox *box, const QString &name) {
    box->setObjectName(name);
    box->setProperty("compactLanguage", true);
    box->setMinimumWidth(90);
    box->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    box->setMinimumContentsLength(8);
    box->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
}
static QString translatedRoute(QComboBox *source, const QJsonObject &value) {
    const QString detected = value["source"].toString();
    const bool automatic = languageValue(source) == "auto";
    const bool known = !detected.isEmpty() && detected != "auto";
    if (automatic) {
        const QSignalBlocker blocker(source);
        source->setItemText(0, known ? UiText::tr("Auto · %1").arg(detected.toUpper())
                                     : UiText::tr("Auto"));
        source->setToolTip(
            known ? UiText::tr("Detected source: %1. Choose a language to correct this estimate.")
                        .arg(detected.toUpper())
                  : UiText::tr("Detect the source language, or choose it yourself."));
    }
    return (automatic && known ? UiText::tr("Detected %1 → %2") : UiText::tr("%1 → %2"))
        .arg(known ? detected.toUpper() : UiText::tr("Auto"), value["target"].toString().toUpper());
}

class NumberField : public QSpinBox {
    void insetText() { lineEdit()->setTextMargins(12, 0, 2, 0); }

  protected:
    void resizeEvent(QResizeEvent *event) override {
        QSpinBox::resizeEvent(event);
        insetText();
    }
    void showEvent(QShowEvent *event) override {
        QSpinBox::showEvent(event);
        insetText();
    }

  public:
    NumberField() { insetText(); }
};

// Keep each label and field together, including when provider-specific rows hide.
class SettingsForm : public QFormLayout {
    QHash<QWidget *, QWidget *> rows;

  public:
    explicit SettingsForm(QWidget *parent) : QFormLayout(parent) {
        setFieldGrowthPolicy(AllNonFixedFieldsGrow);
        setVerticalSpacing(8);
        setFormAlignment(Qt::AlignTop);
    }
    void addRow(QWidget *widget) { QFormLayout::addRow(widget); }
    void addRow(const QString &title, QWidget *field) {
        auto *row = new QWidget;
        auto *layout = new QVBoxLayout(row);
        layout->setContentsMargins(0, 10, 0, 6);
        layout->setSpacing(7);
        auto *caption = label(title, "fieldLabel");
        caption->setBuddy(field);
        layout->addWidget(caption);
        layout->addWidget(field);
        if (qobject_cast<QSpinBox *>(field))
            field->setMaximumWidth(190);
        rows.insert(field, row);
        QFormLayout::addRow(row);
    }
    void setRowVisible(QWidget *field, bool visible) {
        QFormLayout::setRowVisible(rows.value(field, field), visible);
    }
};

// Child cards share the frozen screenshot's surface and stacking order on every compositor.
class SnapshotCard : public GlassSurface {
    QLabel *caption = nullptr, *excerpt = nullptr;
    QWidget *header;
    QPlainTextEdit *result;
    QComboBox *source, *target;
    QPushButton *copy, *retry, *reader;
    QLabel *readerHint;
    TranslationResizeHandle *resizeHandle;
    QColor accent;
    bool moving = false, active = false, reading = false;
    QRect compactGeometry;
    QPoint dragOrigin, originalPosition;
    QString requestedSource = "auto", requestedTarget = "auto";
    void request(bool force = false) {
        const auto nextSource = languageValue(source), nextTarget = languageValue(target);
        if (!force && nextSource == requestedSource && nextTarget == requestedTarget)
            return;
        requestedSource = nextSource;
        requestedTarget = nextTarget;
        if (requested)
            requested(nextSource, nextTarget);
    }

  protected:
    bool eventFilter(QObject *object, QEvent *event) override {
        if (event->type() == QEvent::MouseButtonPress) {
            auto *mouse = static_cast<QMouseEvent *>(event);
            if (activated)
                activated();
            if (!reading && mouse->button() == Qt::LeftButton &&
                (object == header || (qobject_cast<QLabel *>(object) &&
                                      header->isAncestorOf(qobject_cast<QWidget *>(object))))) {
                moving = true;
                dragOrigin = mouse->globalPosition().toPoint();
                originalPosition = pos();
                header->grabMouse();
                header->setCursor(Qt::ClosedHandCursor);
                return true;
            }
        } else if (moving && event->type() == QEvent::MouseMove) {
            auto *mouse = static_cast<QMouseEvent *>(event);
            move(originalPosition + mouse->globalPosition().toPoint() - dragOrigin);
            if (moved)
                moved();
            return true;
        } else if (moving && event->type() == QEvent::MouseButtonRelease) {
            auto *mouse = static_cast<QMouseEvent *>(event);
            move(originalPosition + mouse->globalPosition().toPoint() - dragOrigin);
            if (moved)
                moved();
            moving = false;
            header->releaseMouse();
            header->setCursor(Qt::OpenHandCursor);
            return true;
        }
        return GlassSurface::eventFilter(object, event);
    }
    void resizeEvent(QResizeEvent *event) override {
        GlassSurface::resizeEvent(event);
        if (excerpt)
            excerpt->setText(excerpt->fontMetrics().elidedText(excerpt->toolTip().simplified(),
                                                               Qt::ElideRight, width() - 32));
        if (moved)
            moved();
    }
    void paintEvent(QPaintEvent *event) override {
        GlassSurface::paintEvent(event);
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QColor edge = accent;
        edge.setAlpha(active ? 215 : 95);
        p.setPen(QPen(edge, active ? 1.6 : 1));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 22, 22);
    }

  public:
    std::function<void()> activated, moved, dismissed;
    std::function<void(bool)> readingChanged;
    std::function<void(QString, QString)> requested;
    SnapshotCard(QWidget *parent, int number, QColor color) : GlassSurface(parent), accent(color) {
        setObjectName(QString("snapshotCard%1").arg(number));
        setAccessibleName(QString(UiText::tr("Snapshot translation %1")).arg(number));
        setAttribute(Qt::WA_NoMousePropagation);
        setCursor(Qt::ArrowCursor);
        setProperty("overlayToolbar", true);
        setMinimumSize(360, 220);
        resize(400, 248);
        auto *shadow = new QGraphicsDropShadowEffect(this);
        shadow->setBlurRadius(24);
        shadow->setOffset(0, 6);
        shadow->setColor(QColor(5, 15, 35, 65));
        setGraphicsEffect(shadow);
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(16, 14, 16, 14);
        layout->setSpacing(8);
        header = new QWidget(this);
        header->setObjectName("cardDragHandle");
        header->setAccessibleName(UiText::tr("Drag translation card"));
        header->setCursor(Qt::OpenHandCursor);
        auto *row = new QHBoxLayout(header);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(8);
        auto *badge = label(QString::number(number));
        badge->setAlignment(Qt::AlignCenter);
        badge->setFixedSize(26, 26);
        badge->setStyleSheet(
            QString("background:%1; color:white; border-radius:13px; font-weight:600;")
                .arg(accent.name()));
        row->addWidget(badge);
        caption = label(UiText::tr("TRANSLATION"), "eyebrow");
        row->addWidget(caption, 1);
        reader = iconButton("expand", UiText::tr("Read this translation full screen"));
        row->addWidget(reader);
        auto *close = iconButton("close", UiText::tr("Close this translation"));
        row->addWidget(close);
        layout->addWidget(header);
        excerpt = label(UiText::tr("Reading selected area…"), "muted");
        excerpt->setFixedHeight(20);
        layout->addWidget(excerpt);
        readerHint = label(UiText::tr("Shift or Esc to return to the card"), "muted");
        readerHint->hide();
        layout->addWidget(readerHint);
        result = new QPlainTextEdit;
        result->setObjectName("snapshotTranslation");
        result->setAccessibleName(QString(UiText::tr("Translated text %1")).arg(number));
        result->setReadOnly(true);
        result->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        result->setFrameShape(QFrame::NoFrame);
        result->setStyleSheet("font-size:17px;");
        result->setPlaceholderText(UiText::tr("Waiting to translate…"));
        layout->addWidget(result, 1);
        auto *footer = new QHBoxLayout;
        footer->setSpacing(6);
        source = languages(true);
        compactLanguage(source, "snapshotSourceLanguage");
        source->setItemText(0, UiText::tr("Auto"));
        footer->addWidget(source, 1);
        footer->addWidget(label("→", "muted"));
        target = languages(false);
        compactLanguage(target, "snapshotTargetLanguage");
        target->setItemText(0, UiText::tr("Auto target"));
        footer->addWidget(target, 1);
        retry = button(UiText::tr("Retry"));
        retry->hide();
        footer->addWidget(retry);
        copy = iconButton("copy", UiText::tr("Copy this translation"));
        copy->setEnabled(false);
        footer->addWidget(copy);
        resizeHandle = new TranslationResizeHandle(this);
        footer->addWidget(resizeHandle, 0, Qt::AlignBottom);
        layout->addLayout(footer);
        connect(copy, &QPushButton::clicked, this,
                [this] { QApplication::clipboard()->setText(result->toPlainText()); });
        connect(close, &QPushButton::clicked, this, [this] {
            if (dismissed)
                dismissed();
        });
        connect(retry, &QPushButton::clicked, this, [this] { request(true); });
        connect(reader, &QPushButton::clicked, this, [this] { setReading(!reading); });
        for (auto *language : {source, target}) {
            connect(language, &QComboBox::activated, this, [this] { request(); });
            connect(language->lineEdit(), &QLineEdit::editingFinished, this, [this] { request(); });
        }
        installEventFilter(this);
        for (auto *child : findChildren<QWidget *>())
            child->installEventFilter(this);
        new ReaderKeyFilter(this, [this] { return reading; }, [this] { setReading(false); });
    }
    bool isReading() const { return reading; }
    void focusReadingText() { result->setFocus(); }
    void fitReader() {
        if (reading && parentWidget())
            setGeometry(parentWidget()->rect().adjusted(16, 16, -16, -16));
    }
    void setReading(bool expanded) {
        if (reading == expanded)
            return;
        if (expanded)
            compactGeometry = geometry();
        reading = expanded;
        if (readingChanged)
            readingChanged(reading);
        setProperty("expandedReader", reading);
        readerHint->setVisible(reading);
        resizeHandle->setVisible(!reading);
        header->setCursor(reading ? Qt::ArrowCursor : Qt::OpenHandCursor);
        reader->setAccessibleName(reading ? UiText::tr("Return to translation card")
                                          : UiText::tr("Read this translation full screen"));
        reader->setToolTip(reader->accessibleName());
        result->setStyleSheet(reading ? "font-size:22px;" : "font-size:17px;");
        if (reading)
            fitReader();
        else
            setGeometry(compactGeometry);
        if (activated)
            activated();
        result->setFocus();
    }
    void setActive(bool value) {
        active = value;
        update();
    }
    void setSource(const QString &text) {
        excerpt->setText(
            excerpt->fontMetrics().elidedText(text.simplified(), Qt::ElideRight, width() - 32));
        excerpt->setToolTip(text);
        setAccessibleDescription(text);
    }
    void busy() {
        result->clear();
        result->setPlaceholderText(UiText::tr("Waiting to translate…"));
        caption->setText(UiText::tr("TRANSLATION"));
        copy->setEnabled(false);
        retry->hide();
    }
    void display(const QJsonObject &value) {
        const bool error = value["phase"].toString() == "error";
        result->setFont(Design::textFont(result->font(), value["target"].toString()));
        result->setPlainText(value["text"].toString());
        caption->setText(error ? UiText::tr("COULD NOT TRANSLATE")
                               : translatedRoute(source, value));
        copy->setEnabled(!error);
        retry->setVisible(error);
    }
};

class SnapshotView : public QWidget {
    QImage image;
    QJsonArray words;
    QPointF start, end;
    bool dragging = false;
    bool ready = false, pending = false, submitted = false;
    QPointF hover;
    QLabel *hint = nullptr;
    std::function<void(QImage, QPoint)> recognizeRegion;
    bool copySelection = false;
    bool panning = false;
    QPointF pan, panStart;
    double zoom = 1.0;
    QLabel *zoomLabel = nullptr;
    QPushButton *selectAllAction = nullptr, *translateAllAction = nullptr;
    QSet<int> chosen;
    struct Region {
        SnapshotCard *card;
        QColor color;
        QJsonArray boxes;
        QRectF bounds;
        QJsonObject request;
        int requestId = 0;
    };
    QMap<int, Region> regions;
    QString session = QUuid::createUuid().toString(QUuid::Id128);
    QTemporaryDir regionShots;
    bool multiple = false, keepSelection = false;
    int nextCard = 0, nextRequest = 0, activeCard = 0;
    QWidget *toolbar = nullptr;
    QRectF mapImageRect(const QRectF &raw) const {
        const auto area = imageRect();
        const double scale = area.width() / image.width();
        return QRectF(area.topLeft() + raw.topLeft() * scale, raw.size() * scale);
    }
    static QRectF rawWordRect(const QJsonObject &word) {
        return {word["x"].toDouble(), word["y"].toDouble(), word["width"].toDouble(),
                word["height"].toDouble()};
    }
    QRectF rawSelection() const {
        const auto area = imageRect();
        const double scale = image.width() / area.width();
        const auto selection = QRectF(start, end).normalized();
        return {(selection.topLeft() - area.topLeft()) * scale, selection.size() * scale};
    }
    void updateHint() {
        hint->setText(
            multiple ? UiText::tr("Select more text · Drag cards · Esc to leave")
                     : UiText::tr(
                           "Select text · Hold Shift or Ctrl to keep the snapshot · Esc to leave"));
    }
    QPoint boundedPosition(QPoint point, QSize size) const {
        const int top = toolbar ? toolbar->geometry().bottom() + 12 : 90;
        return {qBound(8, point.x(), qMax(8, width() - size.width() - 8)),
                qBound(top, point.y(), qMax(top, height() - size.height() - 8))};
    }
    void activateCard(int id) {
        activeCard = id;
        for (auto it = regions.begin(); it != regions.end(); ++it)
            it->card->setActive(it.key() == id);
        if (regions.contains(id))
            regions[id].card->raise();
        update();
    }
    bool restoreReader() {
        for (auto &region : regions) {
            if (region.card->isReading()) {
                region.card->setReading(false);
                return true;
            }
        }
        return false;
    }
    SnapshotCard *expandedReader() const {
        for (const auto &region : regions)
            if (region.card->isReading())
                return region.card;
        return nullptr;
    }
    void requestRegion(int id, const QString &source, const QString &target) {
        if (!regions.contains(id))
            return;
        auto &region = regions[id];
        if (region.requestId)
            send({{"type", "snapshot_cancel_region"},
                  {"session", session},
                  {"id", region.requestId}});
        region.requestId = ++nextRequest;
        region.request["type"] = "snapshot_request";
        region.request["session"] = session;
        region.request["id"] = region.requestId;
        region.request["source"] = source;
        region.request["target"] = target;
        region.card->busy();
        send(region.request);
    }
    void addRegion(QString text, QJsonArray boxes, QRectF bounds, QJsonObject request = {}) {
        multiple = true;
        updateHint();
        static const QColor colors[] = {QColor("#477edb"), QColor("#ad59be"), QColor("#258b80"),
                                        QColor("#bf7c2f"), QColor("#ce597a"), QColor("#6474bd")};
        const int id = ++nextCard;
        auto color = colors[(id - 1) % 6];
        auto *card = new SnapshotCard(this, id, color);
        request["text"] = text;
        regions.insert(id, {card, color, boxes, bounds, request, 0});
        if (!text.isEmpty()) {
            card->setSource(text);
            if (copySelection)
                QApplication::clipboard()->setText(text);
        }
        card->activated = [this, id] { activateCard(id); };
        card->moved = [this, card] {
            if (!card->isReading())
                card->move(boundedPosition(card->pos(), card->size()));
            update();
        };
        card->readingChanged = [this, id](bool reading) {
            for (auto it = regions.begin(); it != regions.end(); ++it)
                if (it.key() != id)
                    it->card->setEnabled(!reading);
            toolbar->setEnabled(!reading);
        };
        card->requested = [this, id](QString source, QString target) {
            requestRegion(id, source, target);
        };
        card->dismissed = [this, id] {
            if (regions[id].card->isReading())
                regions[id].card->setReading(false);
            auto region = regions.take(id);
            send({{"type", "snapshot_cancel_region"},
                  {"session", session},
                  {"id", region.requestId}});
            region.card->hide();
            region.card->deleteLater();
            setFocus();
            if (!regions.isEmpty())
                activateCard(regions.lastKey());
            update();
        };
        const QRectF anchor = mapImageRect(bounds);
        const QList<QPoint> candidates = {
            anchor.bottomRight().toPoint() + QPoint(20, 14),
            anchor.topRight().toPoint() + QPoint(20, -card->height()),
            anchor.bottomLeft().toPoint() + QPoint(-card->width() - 20, 14),
            anchor.topLeft().toPoint() - QPoint(card->width() + 20, card->height() + 14),
            QPoint(width() - card->width() - 24, 100 + (id - 1) * 32),
            QPoint(24, height() - card->height() - 24)};
        double best = std::numeric_limits<double>::max();
        QPoint position;
        for (auto candidate : candidates) {
            const QPoint preferred = candidate;
            candidate = boundedPosition(candidate, card->size());
            QRectF area(candidate, card->size());
            auto overlap = area.intersected(anchor);
            double score = overlap.width() * overlap.height() * 2;
            for (auto it = regions.cbegin(); it != regions.cend(); ++it) {
                if (it.key() == id)
                    continue;
                overlap = area.intersected(it->card->geometry());
                score += overlap.width() * overlap.height();
            }
            for (int i = 0; i < words.size(); ++i) {
                overlap = area.intersected(wordRect(i));
                score += overlap.width() * overlap.height() * .2;
            }
            // Avoid clamping a left-hand candidate back over the source's text column.
            score += QLineF(preferred, candidate).length() * 10;
            score += QLineF(anchor.center(), area.center()).length() * .01;
            if (score < best) {
                best = score;
                position = candidate;
            }
        }
        card->move(position);
        card->show();
        activateCard(id);
        chosen.clear();
        requestRegion(id, "auto", "auto");
    }
    QRectF imageRect() const {
        QSizeF size = image.size();
        size.scale(QSizeF(width(), height()), Qt::KeepAspectRatio);
        size *= zoom;
        return QRectF((width() - size.width()) / 2 + pan.x(),
                      (height() - size.height()) / 2 + pan.y(), size.width(), size.height());
    }
    void setZoom(double value, QPointF anchor) {
        const auto before = imageRect();
        const QPointF fraction((anchor.x() - before.x()) / before.width(),
                               (anchor.y() - before.y()) / before.height());
        zoom = qBound(1.0, value, 8.0);
        const auto after = imageRect();
        pan += anchor - QPointF(after.x() + fraction.x() * after.width(),
                                after.y() + fraction.y() * after.height());
        if (zoom == 1.0)
            pan = {};
        zoomLabel->setText(QString::number(qRound(zoom * 100)) + "%");
        update();
    }
    QRectF wordRect(int i) const { return mapImageRect(rawWordRect(words[i].toObject())); }
    void choose() {
        chosen.clear();
        QRectF region(start, end);
        region = region.normalized();
        for (int i = 0; i < words.size(); ++i) {
            const auto box = wordRect(i);
            if ((region.width() < 5 && region.height() < 5)
                    ? box.adjusted(-3, -3, 3, 3).contains(end)
                    : region.intersects(box))
                chosen.insert(i);
        }
    }
    void finish() {
        QString text, line;
        QJsonArray boxes;
        QRectF bounds;
        for (int i = 0; i < words.size(); ++i)
            if (chosen.contains(i)) {
                auto word = words[i].toObject();
                QString next = word["line"].toString();
                if (!text.isEmpty())
                    text += next == line ? " " : "\n";
                text += word["text"].toString();
                line = next;
                boxes.append(word);
                bounds = bounds.united(rawWordRect(word));
            }
        if (text.isEmpty())
            return;
        if (multiple || keepSelection) {
            addRegion(text, boxes, bounds);
            return;
        }
        if (submitted)
            return;
        submitted = true;
        if (copySelection)
            QApplication::clipboard()->setText(text);
        hint->setText(UiText::tr("Translating…"));
        send({{"type", "ocr_selected"}, {"text", text}});
    }
    void paintRegion(QPainter &p, int id, const Region &region) {
        QColor color = region.color.lighter(125);
        color.setAlpha(id == activeCard ? 255 : 195);
        p.setPen(QPen(color, id == activeCard ? 3 : 2, Qt::SolidLine, Qt::RoundCap));
        for (const auto &box : region.boxes) {
            const auto r = mapImageRect(rawWordRect(box.toObject()));
            p.drawLine(r.bottomLeft() + QPointF(0, 3), r.bottomRight() + QPointF(0, 3));
        }
        auto source = mapImageRect(region.bounds);
        if (region.boxes.isEmpty())
            p.drawLine(source.bottomLeft(), source.bottomRight());
        const QRectF card = region.card->geometry();
        // Nearest edges keep the pointer short, including after zooming, panning or dragging.
        const auto nearest = [](QRectF rect, QPointF point) {
            QPointF p(qBound(rect.left(), point.x(), rect.right()),
                      qBound(rect.top(), point.y(), rect.bottom()));
            if (rect.contains(point)) {
                const double distances[] = {point.x() - rect.left(), rect.right() - point.x(),
                                            point.y() - rect.top(), rect.bottom() - point.y()};
                int edge =
                    int(std::min_element(std::begin(distances), std::end(distances)) - distances);
                if (edge == 0)
                    p.setX(rect.left());
                else if (edge == 1)
                    p.setX(rect.right());
                else if (edge == 2)
                    p.setY(rect.top());
                else
                    p.setY(rect.bottom());
            }
            return p;
        };
        QPointF from = nearest(source, card.center()), to = nearest(card, from);
        QPainterPath line(from);
        const double mid = (from.x() + to.x()) / 2;
        line.cubicTo(QPointF(mid, from.y()), QPointF(mid, to.y()), to);
        p.setPen(QPen(color, id == activeCard ? 1.8 : 1.2));
        p.setBrush(Qt::NoBrush);
        p.drawPath(line);
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        p.drawEllipse(from, 3, 3);
        // Numbers preserve the association when colors repeat or cannot be distinguished.
        QRectF badge(source.topLeft() - QPointF(25, 2), QSizeF(21, 21));
        p.setBrush(region.color);
        p.drawEllipse(badge);
        p.setPen(Qt::white);
        p.drawText(badge, Qt::AlignCenter, QString::number(id));
    }

  protected:
    void closeEvent(QCloseEvent *event) override {
        send({{"type", "snapshot_end"}, {"session", session}});
        QWidget::closeEvent(event);
    }
    void resizeEvent(QResizeEvent *event) override {
        QWidget::resizeEvent(event);
        for (auto &region : regions) {
            if (region.card->isReading())
                region.card->fitReader();
            else
                region.card->move(boundedPosition(region.card->pos(), region.card->size()));
        }
    }
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(rect(), QColor(17, 20, 30));
        p.drawImage(imageRect(), image);
        QRadialGradient shade(hover, 145);
        shade.setColorAt(0, QColor(8, 15, 30, 0));
        shade.setColorAt(.48, QColor(8, 15, 30, 20));
        shade.setColorAt(1, QColor(8, 15, 30, 110));
        p.fillRect(rect(), shade);
        // A quiet halo follows the cursor without continuous animation or GPU work.
        QRadialGradient halo(hover, 72);
        halo.setColorAt(0, QColor(145, 194, 255, 24));
        halo.setColorAt(1, Qt::transparent);
        p.fillRect(QRectF(hover - QPointF(72, 72), QSizeF(144, 144)), halo);
        for (int i = 0; i < words.size(); ++i) {
            QRectF r = wordRect(i).adjusted(-2, -1, 2, 1);
            if (!chosen.contains(i) && !r.adjusted(-35, -35, 35, 35).contains(hover))
                continue;
            p.setPen(
                QPen(chosen.contains(i) ? QColor(106, 175, 255) : QColor(255, 255, 255, 100), 1));
            p.setBrush(chosen.contains(i) ? QColor(50, 130, 255, 100) : Qt::transparent);
            p.drawRoundedRect(r, 2, 2);
        }
        if (dragging) {
            p.setPen(QPen(QColor(120, 180, 255), 1));
            p.setBrush(QColor(70, 130, 255, 30));
            p.drawRect(QRectF(start, end).normalized());
        }
        for (auto it = regions.cbegin(); it != regions.cend(); ++it)
            if (it.key() != activeCard)
                paintRegion(p, it.key(), *it);
        if (regions.contains(activeCard))
            paintRegion(p, activeCard, regions[activeCard]);
    }
    void mousePressEvent(QMouseEvent *e) override {
        if (auto *reader = expandedReader()) {
            reader->focusReadingText();
            e->accept();
            return;
        }
        if (e->button() == Qt::MiddleButton || e->button() == Qt::RightButton) {
            panning = true;
            panStart = e->position();
            setCursor(Qt::ClosedHandCursor);
            return;
        }
        if (e->button() != Qt::LeftButton)
            return;
        setFocus();
        keepSelection = e->modifiers().testFlag(Qt::ShiftModifier) ||
                        e->modifiers().testFlag(Qt::ControlModifier);
        dragging = true;
        start = end = e->position();
        choose();
        update();
    }
    void mouseMoveEvent(QMouseEvent *e) override {
        if (expandedReader()) {
            e->accept();
            return;
        }
        hover = e->position();
        update();
        if (panning) {
            pan += e->position() - panStart;
            panStart = e->position();
            update();
            return;
        }
        if (dragging) {
            keepSelection = keepSelection || e->modifiers().testFlag(Qt::ShiftModifier) ||
                            e->modifiers().testFlag(Qt::ControlModifier);
            end = e->position();
            choose();
            update();
        }
    }
    void mouseReleaseEvent(QMouseEvent *e) override {
        if (expandedReader()) {
            e->accept();
            return;
        }
        if (panning && (e->button() == Qt::MiddleButton || e->button() == Qt::RightButton)) {
            panning = false;
            setCursor(Qt::IBeamCursor);
            return;
        }
        if (e->button() != Qt::LeftButton || !dragging)
            return;
        end = e->position();
        keepSelection = keepSelection || e->modifiers().testFlag(Qt::ShiftModifier) ||
                        e->modifiers().testFlag(Qt::ControlModifier);
        choose();
        dragging = false;
        update();
        if (!ready || chosen.isEmpty()) {
            const auto area = imageRect();
            const double scale = image.width() / area.width();
            QRectF selection(start, end);
            selection = selection.normalized();
            if (selection.width() < 6 && selection.height() < 6)
                selection = QRectF(end - QPointF(180, 60), QSizeF(360, 120));
            QRect crop =
                QRectF((selection.topLeft() - area.topLeft()) * scale, selection.size() * scale)
                    .toAlignedRect()
                    .adjusted(-12, -12, 12, 12)
                    .intersected(image.rect());
            if (multiple || keepSelection) {
                if (!crop.isEmpty()) {
                    const auto path =
                        regionShots.filePath(QUuid::createUuid().toString(QUuid::Id128) + ".png");
                    if (!image.copy(crop).save(path)) {
                        hint->setText(
                            UiText::tr("Could not prepare this region. Select again to retry."));
                        return;
                    }
                    const QRectF raw = rawSelection();
                    const bool click = QLineF(start, end).length() < 6;
                    // A click needs a small visible anchor until OCR supplies its word bounds.
                    const QRectF anchor = click ? raw.adjusted(-3, -3, 3, 3) : raw;
                    addRegion({}, {}, anchor,
                              {{"path", path},
                               {"offset", QJsonArray{crop.x(), crop.y()}},
                               {"rect", QJsonArray{raw.x(), raw.y(), raw.width(), raw.height()}},
                               {"click", click}});
                }
                return;
            }
            pending = true;
            if (!crop.isEmpty() && recognizeRegion) {
                ready = false;
                hint->setText(UiText::tr("Reading selected area…"));
                recognizeRegion(image.copy(crop), crop.topLeft());
            }
        } else
            finish();
    }
    void wheelEvent(QWheelEvent *e) override {
        if (expandedReader()) {
            e->accept();
            return;
        }
        setZoom(zoom * std::pow(1.0015, e->angleDelta().y()), e->position());
        e->accept();
    }
    void keyPressEvent(QKeyEvent *e) override {
        if (expandedReader() && e->key() != Qt::Key_Escape) {
            e->accept();
            return;
        }
        if (e->key() == Qt::Key_Escape) {
            if (auto *popup = QApplication::activePopupWidget()) {
                popup->close();
                return;
            }
            if (restoreReader())
                return;
            close();
            send({{"type", "cancel"}});
        }
        if (e->matches(QKeySequence::SelectAll)) {
            for (int i = 0; i < words.size(); ++i)
                chosen.insert(i);
            update();
        }
        if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
            keepSelection = e->modifiers().testFlag(Qt::ShiftModifier) ||
                            e->modifiers().testFlag(Qt::ControlModifier);
            finish();
        }
    }

  public:
    void receiveRegion(const QJsonObject &value) {
        if (value["session"].toString() != session)
            return;
        const int requestId = value["id"].toInt();
        for (auto &region : regions) {
            if (region.requestId != requestId)
                continue;
            if (value["phase"].toString() == "source") {
                const QString text = value["text"].toString();
                region.request["text"] = text;
                region.boxes = value["words"].toArray();
                region.bounds = {};
                for (const auto &word : region.boxes)
                    region.bounds = region.bounds.united(rawWordRect(word.toObject()));
                region.card->setSource(text);
                if (copySelection)
                    QApplication::clipboard()->setText(text);
                update();
            } else
                region.card->display(value);
            return;
        }
    }
    void setWords(QJsonArray data, QPoint offset = {}) {
        words = data;
        for (int i = 0; i < words.size(); ++i) {
            auto w = words[i].toObject();
            w["x"] = w["x"].toInt() + offset.x();
            w["y"] = w["y"].toInt() + offset.y();
            words[i] = w;
        }
        ready = true;
        selectAllAction->setEnabled(!words.isEmpty());
        translateAllAction->setEnabled(!words.isEmpty());
        QStringList recognized;
        for (auto w : words)
            recognized << w.toObject()["text"].toString();
        setAccessibleDescription(recognized.join(' '));
        updateHint();
        if (pending || dragging) {
            choose();
            if (pending && !dragging)
                finish();
        }
        pending = false;
        update();
    }
    void showError(const QString &text) {
        hint->setText(UiText::tr("%1 · Select an area to retry").arg(text));
        ready = false;
        submitted = false;
    }
    SnapshotView(QImage img, bool copy, std::function<void(QImage, QPoint)> recognize,
                 QScreen *targetScreen = nullptr)
        : image(std::move(img)), recognizeRegion(std::move(recognize)), copySelection(copy) {
        setAttribute(Qt::WA_DeleteOnClose);
        setWindowTitle(UiText::tr("Ranslator — select text"));
        QStringList recognized;
        for (auto entry : words)
            recognized << entry.toObject()["text"].toString();
        setAccessibleDescription(recognized.join(' '));
        setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        auto *controls = new QVBoxLayout;
        controls->setContentsMargins(18, 10, 18, 10);
        controls->setSpacing(8);
        hint = label(UiText::tr("Select text · Reading screen…"), "muted");
        hint->setWordWrap(true);
        hint->setAlignment(Qt::AlignCenter);
        controls->addWidget(hint);
        auto *actions = new QHBoxLayout;
        actions->setSpacing(8);
        auto *less = button("−"), *more = button("+");
        less->setAccessibleName(UiText::tr("Zoom out"));
        more->setAccessibleName(UiText::tr("Zoom in"));
        zoomLabel = label("100%");
        auto *fit = button(UiText::tr("Fit"));
        auto *all = button(UiText::tr("Select all"));
        selectAllAction = all;
        all->setEnabled(false);
        auto *translate = button(UiText::tr("Translate selection"));
        auto *translateAll = button(UiText::tr("Translate all"), "primary");
        translateAllAction = translateAll;
        translateAll->setEnabled(false);
        auto *cancel = button(UiText::tr("Cancel"));
        auto *advanced = button(UiText::tr("Zoom"));
        advanced->setAccessibleName(UiText::tr("Snapshot zoom controls"));
        advanced->setCheckable(true);
        for (auto *action : {all, translate, translateAll, advanced, cancel})
            actions->addWidget(action);
        controls->addLayout(actions);
        auto *zoomControls = new QWidget;
        auto *zoomActions = new QHBoxLayout(zoomControls);
        zoomActions->setContentsMargins(0, 0, 0, 0);
        zoomActions->addStretch();
        for (QWidget *action : QList<QWidget *>{less, zoomLabel, more, fit})
            zoomActions->addWidget(action);
        zoomActions->addStretch();
        controls->addWidget(zoomControls);
        zoomControls->hide();
        connect(advanced, &QPushButton::toggled, zoomControls, &QWidget::setVisible);
        auto *bar = new GlassSurface;
        toolbar = bar;
        bar->setProperty("overlayToolbar", true);
        bar->setObjectName("snapshotBar");
        bar->setLayout(controls);
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(24, 24, 24, 24);
        layout->addWidget(bar, 0, Qt::AlignHCenter);
        layout->addStretch();
        connect(less, &QPushButton::clicked, this,
                [this] { setZoom(zoom / 1.4, rect().center()); });
        connect(more, &QPushButton::clicked, this,
                [this] { setZoom(zoom * 1.4, rect().center()); });
        connect(fit, &QPushButton::clicked, this, [this] { setZoom(1, rect().center()); });
        connect(all, &QPushButton::clicked, this, [this] {
            for (int i = 0; i < words.size(); ++i)
                chosen.insert(i);
            update();
        });
        connect(translate, &QPushButton::clicked, this, [this] {
            const auto modifiers = QApplication::keyboardModifiers();
            keepSelection =
                modifiers.testFlag(Qt::ShiftModifier) || modifiers.testFlag(Qt::ControlModifier);
            finish();
        });
        connect(translateAll, &QPushButton::clicked, this, [this] {
            for (int i = 0; i < words.size(); ++i)
                chosen.insert(i);
            const auto modifiers = QApplication::keyboardModifiers();
            keepSelection =
                modifiers.testFlag(Qt::ShiftModifier) || modifiers.testFlag(Qt::ControlModifier);
            finish();
        });
        connect(cancel, &QPushButton::clicked, this, [this] {
            close();
            send({{"type", "cancel"}});
        });
        setMouseTracking(true);
        hover = mapFromGlobal(QCursor::pos());
        setCursor(Qt::IBeamCursor);
        setFocusPolicy(Qt::StrongFocus);
        setAccessibleName(
            UiText::tr("Screenshot text selection. Hold Shift or Control while selecting for "
                       "multiple translations. "
                       "Control A selects all; Enter translates; Escape cancels."));
        auto *escape = new QShortcut(QKeySequence(Qt::Key_Escape), this);
        escape->setContext(Qt::WidgetWithChildrenShortcut);
        connect(escape, &QShortcut::activated, this, [this] {
            if (auto *popup = QApplication::activePopupWidget()) {
                popup->close();
                return;
            }
            if (restoreReader())
                return;
            close();
            send({{"type", "cancel"}});
        });
        Capture::showOnScreen(this, targetScreen ? targetScreen : Capture::fallbackScreen(), true);
        activateWindow();
        setFocus();
        send({{"type", "snapshot_start"}, {"session", session}});
    }
};

class QuickPopup : public GlassWindow {
    QPlainTextEdit *result;
    QLabel *caption, *readerHint;
    QPushButton *copy, *retry, *reader;
    TranslationResizeHandle *resizeHandle;
    QComboBox *source, *target;
    QString requestedSource = "auto", requestedTarget = "auto";
    bool opening = false, reading = false, userSized = false, programmaticSize = false;
    QRect compactGeometry;
    std::function<void()> dismissed;

  protected:
    void resizeEvent(QResizeEvent *event) override {
        GlassWindow::resizeEvent(event);
        if (isVisible() && event->oldSize().isValid() && !reading && !programmaticSize)
            userSized = true;
    }
    bool event(QEvent *e) override {
        if (e->type() == QEvent::WindowDeactivate && isVisible() && !opening) {
            QTimer::singleShot(0, this, [this] {
                if (!isVisible() || isActiveWindow() || QApplication::activePopupWidget())
                    return;
                hide();
                if (dismissed)
                    dismissed();
            });
        }
        return GlassWindow::event(e);
    }
    void keyPressEvent(QKeyEvent *e) override {
        if (e->key() == Qt::Key_Escape) {
            if (auto *popup = QApplication::activePopupWidget()) {
                popup->close();
                return;
            }
            hide();
            if (dismissed)
                dismissed();
        } else
            GlassWindow::keyPressEvent(e);
    }

  public:
    QuickPopup(QWidget *owner, std::function<void()> expand,
               std::function<void(QString, QString)> changeLanguages, std::function<void()> dismiss)
        : GlassWindow(owner), dismissed(std::move(dismiss)) {
        setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        setWindowTitle(UiText::tr("Quick translation"));
        resize(450, 240);
        setMinimumSize(400, 220);
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(38, 32, 38, 34);
        layout->setSpacing(12);
        auto *header = new QHBoxLayout;
        header->setSpacing(8);
        caption = label(UiText::tr("TRANSLATION"), "eyebrow");
        header->addWidget(caption, 1);
        reader = iconButton("expand", UiText::tr("Read translation full screen"));
        header->addWidget(reader);
        auto *expandButton = iconButton("arrow", UiText::tr("Open full translator"));
        auto *close = iconButton("close", UiText::tr("Dismiss translation"));
        header->addWidget(expandButton);
        header->addWidget(close);
        layout->addLayout(header);
        readerHint = label(UiText::tr("Shift or Esc to return to the widget"), "muted");
        readerHint->hide();
        layout->addWidget(readerHint);
        result = new QPlainTextEdit;
        result->setReadOnly(true);
        result->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        result->setObjectName("quickTranslation");
        result->setAccessibleName(UiText::tr("Quick translated text"));
        result->setFrameShape(QFrame::NoFrame);
        result->setPlaceholderText(UiText::tr("Translating…"));
        layout->addWidget(result, 1);
        auto *footer = new QHBoxLayout;
        footer->setSpacing(6);
        source = languages(true);
        compactLanguage(source, "quickSourceLanguage");
        source->setItemText(0, UiText::tr("Auto"));
        footer->addWidget(source, 1);
        footer->addWidget(label("→", "muted"));
        target = languages(false);
        compactLanguage(target, "quickTargetLanguage");
        footer->addWidget(target, 1);
        retry = button(UiText::tr("Retry"));
        retry->setAccessibleName(UiText::tr("Retry quick translation"));
        retry->hide();
        footer->addWidget(retry);
        copy = iconButton("copy", UiText::tr("Copy quick translation"));
        footer->addWidget(copy);
        resizeHandle = new TranslationResizeHandle(this);
        footer->addWidget(resizeHandle, 0, Qt::AlignBottom);
        layout->addLayout(footer);
        connect(copy, &QPushButton::clicked, this,
                [this] { QApplication::clipboard()->setText(result->toPlainText()); });
        connect(close, &QPushButton::clicked, this, [this] {
            hide();
            if (dismissed)
                dismissed();
        });
        connect(expandButton, &QPushButton::clicked, this, [this, expand] {
            opening = true;
            hide();
            expand();
            opening = false;
        });
        connect(reader, &QPushButton::clicked, this, [this] { setReading(!reading); });
        auto changed = [this, changeLanguages] {
            const auto nextSource = languageValue(source), nextTarget = languageValue(target);
            if (nextSource == requestedSource && nextTarget == requestedTarget)
                return;
            requestedSource = nextSource;
            requestedTarget = nextTarget;
            changeLanguages(nextSource, nextTarget);
        };
        for (auto *language : {source, target}) {
            connect(language, &QComboBox::activated, this, changed);
            connect(language->lineEdit(), &QLineEdit::editingFinished, this, changed);
        }
        connect(retry, &QPushButton::clicked, this, [this, changeLanguages] {
            changeLanguages(languageValue(source), languageValue(target));
        });
        new ReaderKeyFilter(this, [this] { return reading; }, [this] { setReading(false); });
    }
    bool isReading() const { return reading; }
    void setReading(bool expanded) {
        if (reading == expanded)
            return;
        const bool wasVisible = isVisible();
        opening = true;
        programmaticSize = true;
        if (expanded)
            compactGeometry = geometry();
        reading = expanded;
        setProperty("expandedReader", reading);
        readerHint->setVisible(reading);
        resizeHandle->setVisible(!reading);
        reader->setAccessibleName(reading ? UiText::tr("Return to translation widget")
                                          : UiText::tr("Read translation full screen"));
        reader->setToolTip(reader->accessibleName());
        if (reading) {
            Capture::showOnScreen(this, screen(), true);
        } else {
            setWindowState(Qt::WindowNoState);
            setGeometry(compactGeometry);
            if (wasVisible)
                show();
        }
        programmaticSize = false;
        result->setFocus();
        QTimer::singleShot(120, this, [this] { opening = false; });
    }
    void begin(const QJsonObject &config, QScreen *targetScreen = nullptr, bool reveal = true) {
        opening = true;
        if (reading) {
            hide();
            setReading(false);
        }
        result->clear();
        result->setPlaceholderText(UiText::tr("Translating…"));
        copy->setEnabled(false);
        retry->hide();
        caption->setText(UiText::tr("TRANSLATION"));
        requestedSource = requestedTarget = "auto";
        source->setItemText(0, UiText::tr("Auto"));
        source->setToolTip(UiText::tr("Detect the source language, or choose it yourself."));
        setLanguage(source, "auto");
        setLanguage(target, "auto");
        target->setItemText(0, UiText::tr("Auto · %1 / %2")
                                   .arg(config["preferred_language"].toString().toUpper(),
                                        config["fallback_language"].toString().toUpper()));
        if (reveal)
            showPrepared(targetScreen, QCursor::pos());
        else
            hide();
    }
    void showPrepared(QScreen *targetScreen, QPoint cursor) {
        opening = true;
        auto *screen = targetScreen ? targetScreen : Capture::fallbackScreen();
        setScreen(screen);
        winId();
        windowHandle()->setScreen(screen);
        if (screen) {
            const auto area = screen->availableGeometry();
            move(qBound(area.left(), cursor.x() + 18, area.right() - width()),
                 qBound(area.top(), cursor.y() + 20, area.bottom() - height()));
        }
        show();
        raise();
        activateWindow();
        QTimer::singleShot(120, this, [this] { opening = false; });
    }
    void busy() {
        result->clear();
        result->setPlaceholderText(UiText::tr("Translating…"));
        copy->setEnabled(false);
        caption->setText(UiText::tr("TRANSLATING"));
        retry->hide();
    }
    void display(const QJsonObject &value) {
        result->setFont(Design::textFont(result->font(), value["target"].toString()));
        result->setPlainText(value["text"].toString());
        caption->setText(translatedRoute(source, value));
        copy->setEnabled(true);
        retry->hide();
        const auto bounds = result->fontMetrics().boundingRect(
            QRect(0, 0, qMax(180, result->viewport()->width() - 8), 2000), Qt::TextWordWrap,
            value["text"].toString());
        if (!reading && !userSized) {
            programmaticSize = true;
            resize(width(), qBound(210, bounds.height() + 175, 440));
            programmaticSize = false;
        }
        Design::fadeIn(result);
    }
    void error(const QString &message) {
        caption->setText(UiText::tr("COULD NOT TRANSLATE"));
        result->setPlainText(message);
        copy->setEnabled(false);
        retry->show();
    }
};

class Window : public GlassWindow {
    QJsonObject config;
    Portal portal;
    Autostart *autostart;
    QAction *autostartAction = nullptr;
    QPointer<QCheckBox> autostartToggle;
    QPointer<QLabel> autostartDetail;
    QPointer<QPushButton> loginSettingsButton;
    QString autostartError;
    X11Shortcuts x11Shortcuts;
    AccessibilitySelection accessibleSelection;
    GnomeDesktop gnomeDesktop;
    QJsonObject pendingGnomeCapture;
    QElapsedTimer pendingGnomeCaptureTime;
    quint64 nativeSelectionGeneration = 0;
    bool nativeSelectionArmed = false;
    quint64 quickPlacementGeneration = 0;
#ifdef Q_OS_MACOS
    MacShortcuts macShortcuts{this};
#endif
    QSystemTrayIcon *tray;
    QLabel *status, *providerLabel, *route;
    QPlainTextEdit *sourceText, *translated;
    QComboBox *sourceLanguage, *targetLanguage;
    QPushButton *translateButton, *continuousButton, *copyButton;
    QPushButton *onceButton, *stopButton;
    QProgressBar *progress;
    QPointer<QDialog> settings;
    QPointer<QLabel> settingsError;
    QPointer<SnapshotView> overlay;
    QuickPopup *quick = nullptr;
    bool quickSession = false;
    bool quickPending = false;
    QString translationRequest;
    QTemporaryDir shots{Capture::temporaryDirectoryTemplate()};
    QList<QAction *> shortcuts;
    QString screenshotPath;
    QPoint snapshotOffset;
    QString translatedSource, translatedTarget;
    bool smokeMode;
    bool readingSelection = false;
    QElapsedTimer ignoreClipboardUntil;
    bool selectionFocus = false;
    unsigned captureGeneration = 0;
    bool captureNeedsFrameDelay = false;
    QPointer<QProcess> captureProcess;
    QPointer<QScreen> captureScreen;
    QPointer<QWidget> captureSelector;
    quint64 settingsRequest = 0;
    std::function<void(QJsonObject)> onModels, onPacks, onRuntimes;
    void syncAutostart() {
        const auto &state = autostart->state();
        const bool busy = autostart->isBusy();
        QString detail =
            busy ? UiText::tr("Waiting for the system… Approve its permission dialog if shown.")
                 : state.detail;
        if (!autostartError.isEmpty() && !busy)
            detail = autostartError + "\n" + detail;
        if (autostartAction) {
            QSignalBlocker blocker(autostartAction);
            autostartAction->setChecked(state.enabled || state.requiresApproval);
            autostartAction->setEnabled(state.supported && !busy);
            autostartAction->setText(busy ? UiText::tr("Start at login — applying…")
                                     : state.requiresApproval
                                         ? UiText::tr("Start at login — approval needed")
                                     : !state.known ? UiText::tr("Start at login — not verified")
                                                    : UiText::tr("Start at login"));
            autostartAction->setToolTip(detail);
        }
        if (autostartToggle) {
            QSignalBlocker blocker(autostartToggle);
            autostartToggle->setChecked(state.enabled || state.requiresApproval);
            autostartToggle->setEnabled(state.supported && !busy);
            autostartToggle->setToolTip(detail);
        }
        if (autostartDetail)
            autostartDetail->setText(detail);
        if (loginSettingsButton)
            loginSettingsButton->setVisible(state.requiresApproval);
    }
    void changeAutostart(bool enabled) {
        autostartError.clear();
        autostart->setEnabled(enabled);
        syncAutostart();
    }
    void updateLanguageHint() {
        const QSignalBlocker blocker(targetLanguage);
        targetLanguage->setItemText(0, UiText::tr("Auto · %1 / %2")
                                           .arg(config["preferred_language"].toString().toUpper(),
                                                config["fallback_language"].toString().toUpper()));
        targetLanguage->setToolTip(UiText::tr("Translate into %1; use %2 when the source already "
                                              "matches. Choose a language to override.")
                                       .arg(config["preferred_language"].toString().toUpper(),
                                            config["fallback_language"].toString().toUpper()));
    }
    void setStatus(QString text) {
        status->setText(text);
        status->setToolTip(text);
    }
    void syncSelectionFocus() {
        const auto *active = QApplication::activeWindow();
        // Wayland may retain ApplicationActive after our last window is hidden. Only an
        // actually visible focused window should suppress text from the selection watcher.
        const bool focused = active && active->isVisible() &&
                             QGuiApplication::applicationState() == Qt::ApplicationActive;
        if (focused == selectionFocus)
            return;
        selectionFocus = focused;
        send({{"type", "focus"}, {"active", focused}});
    }
    void working(bool busy) {
        progress->setVisible(busy);
        translateButton->setText(busy ? UiText::tr("Translate again") : UiText::tr("Translate"));
        stopButton->setEnabled(busy || continuousButton->isChecked() || onceButton->isChecked());
    }
    void showCard() {
        quickSession = false;
        if (quick)
            quick->hide();
        show();
        raise();
        activateWindow();
        sourceText->setFocus(Qt::OtherFocusReason);
        QTimer::singleShot(0, this, [this] {
            if (isVisible() && QApplication::activeWindow() == this)
                sourceText->setFocus(Qt::OtherFocusReason);
        });
    }
    QString readyHint() const {
        return config["translate_selection"].toObject()["sequence"].toString().isEmpty()
                   ? UiText::tr("Select anywhere with %1").arg(bindingDescription("once"))
                   : UiText::tr("Select text, then press %1")
                         .arg(bindingDescription("translate_selection"));
    }
    QString bindingDescription(const char *id) const {
        auto b = config[id].toObject();
        return b["sequence"].toString() +
               (b["taps"].toInt() > 1 ? QString(" ×%1").arg(b["taps"].toInt()) : "");
    }
    void translate() {
        if (sourceText->toPlainText().trimmed().isEmpty()) {
            sourceText->setFocus();
            setStatus(UiText::tr("Select, paste, or type something to translate."));
            return;
        }
        translated->clear();
        copyButton->setEnabled(false);
        translationRequest = QUuid::createUuid().toString(QUuid::Id128);
        send({{"type", "translate"},
              {"request_id", translationRequest},
              {"text", sourceText->toPlainText()},
              {"source", languageValue(sourceLanguage)},
              {"target", languageValue(targetLanguage)}});
    }
    void loadImage(const QString &path, QScreen *targetScreen = nullptr,
                   bool imageMetadata = false) {
        QImageReader reader(path);
        const QSize size = reader.size();
        if (!size.isValid() || qint64(size.width()) * size.height() > 120000000) {
            receive({{"type", "error"},
                     {"text", UiText::tr("Image is invalid or exceeds 120 megapixels.")}});
            return;
        }
        auto img = reader.read();
        if (img.isNull()) {
            receive({{"type", "error"}, {"text", UiText::tr("Could not open screenshot.")}});
            return;
        }
        auto *screen = Capture::imageScreen(
            img, targetScreen ? targetScreen : Capture::fallbackScreen(), imageMetadata);
        if (overlay)
            overlay->close();
        hide();
        snapshotOffset = {};
        screenshotPath = shots.filePath(QUuid::createUuid().toString(QUuid::Id128) + ".png");
        if (!QFile::copy(path, screenshotPath)) {
            setStatus(UiText::tr("Could not prepare screenshot."));
            return;
        }
        overlay = new SnapshotView(
            img, config["snapshot_copy_text"].toBool(),
            [this](QImage crop, QPoint offset) {
                const auto path =
                    shots.filePath(QUuid::createUuid().toString(QUuid::Id128) + ".png");
                if (!crop.save(path))
                    return;
                screenshotPath = path;
                snapshotOffset = offset;
                send({{"type", "ocr"}, {"path", path}});
            },
            screen);
        send({{"type", "ocr"}, {"path", screenshotPath}});
    }
    void capturePortal(unsigned generation) {
        portal.screenshot(
            [this, generation](QString uri, QString error) {
                if (generation != captureGeneration)
                    return;
                if (!error.isEmpty()) {
                    receive({{"type", "error"}, {"text", error}});
                    return;
                }
                QUrl url(uri);
                if (!url.isLocalFile()) {
                    receive(
                        {{"type", "error"},
                         {"text", UiText::tr("Screenshot portal returned an unsupported URI.")}});
                    return;
                }
                loadImage(url.toLocalFile(), captureScreen, true);
            },
            config["capture_mode"].toString("auto"));
    }
    // Translate selection reads what is already selected in the frontmost application.
    void readSelection() {
        auto finish = [this](const QString &text, const QString &error) {
            // A copy fallback changes and then restores the clipboard; neither is a new selection.
            ignoreClipboardUntil.start();
            send(error.isEmpty() ? QJsonObject{{"type", "selection_read"}, {"text", text}}
                                 : QJsonObject{{"type", "selection_read"}, {"error", error}});
        };
#ifdef Q_OS_MACOS
        ignoreClipboardUntil.invalidate();
        readingSelection = true;
        NativeMac::readSelection(this, [this, finish](QString text, QString error) {
            readingSelection = false;
            finish(text, error);
        });
#else
        if (QGuiApplication::platformName() == "xcb")
            finish(QApplication::clipboard()->text(QClipboard::Selection), {});
        else
            finish({}, UiText::tr("This desktop does not share the current selection. Use Select "
                                  "once or Snapshot instead."));
#endif
    }
    bool clipboardChangeIsOurs() const {
        return readingSelection ||
               (ignoreClipboardUntil.isValid() && ignoreClipboardUntil.elapsed() < 1500);
    }
    void capture() {
        captureNeedsFrameDelay =
            isVisible() || (quick && quick->isVisible()) || (overlay && overlay->isVisible());
        ++quickPlacementGeneration;
        quickPending = false;
        quickSession = false;
        if (quick)
            quick->hide();
        const auto generation = ++captureGeneration;
        portal.cancelScreenshots();
        gnomeDesktop.cancelCapture();
        if (captureSelector)
            captureSelector->close();
        if (captureProcess)
            captureProcess->kill();
        if (gnomeDesktop.available() && !Capture::x11Available() &&
            config["capture_mode"].toString("auto") != "picker") {
            auto start = [this, generation](QJsonObject context, const QString &error) {
                if (generation != captureGeneration)
                    return;
                if (!error.isEmpty()) {
                    receive({{"type", "error"}, {"text", error}});
                    return;
                }
                captureScreen = GnomeDesktop::screenForContext(context);
                captureOnScreen(generation, context["index"].toInt(-1));
            };
            if (!pendingGnomeCapture.isEmpty() && pendingGnomeCaptureTime.isValid() &&
                pendingGnomeCaptureTime.elapsed() < 250) {
                auto context = pendingGnomeCapture;
                pendingGnomeCapture = {};
                start(context, {});
            } else {
                pendingGnomeCapture = {};
                gnomeDesktop.context(start);
            }
            return;
        }
        Capture::invocationScreen(this, [this, generation](QScreen *screen) {
            if (generation != captureGeneration)
                return;
            captureScreen = screen;
            captureOnScreen(generation);
        });
    }
    void captureOnScreen(unsigned generation, int gnomeMonitor = -1) {
        const bool wasVisible = isVisible() || captureNeedsFrameDelay;
        hide();
        if (quick)
            quick->hide();
        if (overlay)
            overlay->close();
        QTimer::singleShot(wasVisible ? 40 : 0, this, [this, generation, gnomeMonitor] {
            if (generation != captureGeneration)
                return;
            const auto mode = config["capture_mode"].toString("auto");
            if (gnomeMonitor >= 0 && gnomeDesktop.available()) {
                gnomeDesktop.capture(
                    mode == "desktop" ? "desktop" : "auto", gnomeMonitor,
                    [this, generation, mode](QImage image, QJsonObject context, QString error) {
                        if (generation != captureGeneration)
                            return;
                        if (!error.isEmpty()) {
                            receive({{"type", "error"}, {"text", error}});
                            return;
                        }
                        if (auto *screen = GnomeDesktop::screenForContext(context))
                            captureScreen = screen;
                        auto finish = [this, generation](QImage captured) {
                            if (generation != captureGeneration || captured.isNull())
                                return;
                            const auto path =
                                shots.filePath(QUuid::createUuid().toString(QUuid::Id128) + ".png");
                            if (captured.save(path))
                                loadImage(path, captureScreen);
                            else
                                receive(
                                    {{"type", "error"},
                                     {"text", UiText::tr("Could not save the captured image.")}});
                            QFile::remove(path);
                        };
                        if (mode == "region")
                            captureSelector =
                                Capture::selectRegion(this, image, captureScreen, finish);
                        else
                            finish(std::move(image));
                    });
                return;
            }
#ifdef Q_OS_MACOS
            NativeMac::capture(
                this, mode, captureScreen, [this, generation, mode](QImage image, QString error) {
                    if (generation != captureGeneration)
                        return;
                    if (!error.isEmpty()) {
                        receive({{"type", "error"}, { "text", error }});
                        return;
                    }
                    auto finish = [this, generation](QImage captured) {
                        if (generation != captureGeneration || captured.isNull())
                            return;
                        const auto path =
                            shots.filePath(QUuid::createUuid().toString(QUuid::Id128) + ".png");
                        if (captured.save(path))
                            loadImage(path, captureScreen);
                        else
                            receive({{"type", "error"},
                                     { "text",
                                       UiText::tr("Could not save the captured image.") }});
                        QFile::remove(path);
                    };
                    if (mode == "region" || mode == "picker")
                        captureSelector = Capture::selectRegion(this, image, captureScreen, finish);
                    else
                        finish(std::move(image));
                });
            return;
#endif
            if (mode != "picker" && Capture::x11Available()) {
                captureSelector = Capture::captureX11(
                    this, mode, captureScreen, [this, generation](QImage image) {
                        if (generation != captureGeneration || image.isNull())
                            return;
                        const auto path =
                            shots.filePath(QUuid::createUuid().toString(QUuid::Id128) + ".png");
                        if (image.save(path))
                            loadImage(path, captureScreen);
                        else
                            receive({{"type", "error"},
                                     {"text", UiText::tr("Could not save the captured image.")}});
                        QFile::remove(path);
                    });
                return;
            }
            if (mode == "picker" || !Capture::directCaptureAvailable()) {
                capturePortal(generation);
                return;
            }
            const auto path = shots.filePath(QUuid::createUuid().toString(QUuid::Id128) + ".png");
            auto *process = new QProcess(this);
            captureProcess = process;
            const auto command = Capture::spectacleCommand(mode, path);
            process->setProcessEnvironment(command.environment);
            process->setStandardOutputFile(QProcess::nullDevice());
            process->setStandardErrorFile(QProcess::nullDevice());
            auto complete = [this, generation, path, mode, process](bool failed) {
                if (process->property("handled").toBool())
                    return;
                process->setProperty("handled", true);
                process->deleteLater();
                if (generation == captureGeneration) {
                    if (!failed && QFile::exists(path))
                        loadImage(path, captureScreen, mode != "desktop");
                    else if (mode == "auto")
                        capturePortal(generation);
                    else
                        receive(
                            {{"type", "error"},
                             {"text",
                              UiText::tr("Capture cancelled or unavailable. Try System picker in "
                                         "Settings → Screen text.")}});
                }
                QFile::remove(path);
            };
            connect(process, &QProcess::finished, this,
                    [complete](int code, QProcess::ExitStatus status) {
                        complete(code != 0 || status != QProcess::NormalExit);
                    });
            connect(process, &QProcess::errorOccurred, this,
                    [complete](QProcess::ProcessError error) {
                        if (error == QProcess::FailedToStart)
                            complete(true);
                    });
            QTimer::singleShot(mode == "region" ? 120000 : 15000, process, [process] {
                if (process->state() != QProcess::NotRunning)
                    process->kill();
            });
            process->start(command.program, command.arguments);
        });
    }
    QString validateShortcuts(const QJsonObject &candidate) {
        QSet<QString> used;
        for (const auto &id : shortcutActions) {
            const auto sequence = candidate[id].toObject()["sequence"].toString().trimmed();
            if (id == "translate_selection" && sequence.isEmpty())
                continue;
            if (modifierOnly(sequence))
                continue;
            const auto key = QKeySequence::fromString(sequence, QKeySequence::PortableText);
            if (key.isEmpty() || key.count() != 1 || key[0].key() == Qt::Key_unknown)
                return UiText::tr("Use one key chord, for example Ctrl+Alt+R, or Shift.");
            QString canonical = key.toString(QKeySequence::PortableText);
            if (used.contains(canonical))
                return UiText::tr("Use a different chord for each action.");
            used.insert(canonical);
#ifdef HAVE_KGLOBALACCEL
            if (qEnvironmentVariable("XDG_CURRENT_DESKTOP").contains("KDE") &&
                shortcutConflict(key))
                return UiText::
                    tr("Shortcut %1 is already used by another application. Choose another chord.")
                        .arg(canonical);
#endif
        }
        return {};
    }
    void dispatchShortcut(const QString &id, bool exact = true, QString sequence = {},
                          const QJsonObject &context = {}) {
        if (auto *recorder = ShortcutRecorder::active()) {
            if (exact)
                recorder->acceptRegisteredShortcut(
                    sequence.isEmpty() ? config[id].toObject()["sequence"].toString() : sequence);
            else
                recorder->interceptedShortcut();
            return;
        }
        if (ShortcutRecorder::justFinished())
            return;
        if (id == "snapshot" && !context.isEmpty()) {
            pendingGnomeCapture = context;
            pendingGnomeCaptureTime.start();
        }
        send({{"type", "shortcut"}, {"action", id}});
    }
    void registerShortcuts() {
        if (smokeMode)
            return;
#ifdef Q_OS_MACOS
        macShortcuts.bind(
            config, [this](QString id) { dispatchShortcut(id); },
            [this](QString error) { setStatus(error); });
        return;
#endif
#ifdef HAVE_KGLOBALACCEL
        if (qEnvironmentVariable("XDG_CURRENT_DESKTOP").contains("KDE")) {
            if (x11Shortcuts.available())
                x11Shortcuts.bind(
                    config, [this](QString id) { dispatchShortcut(id); },
                    [this](QString error) { setStatus(error); }, true);
            for (auto *a : shortcuts) {
                KGlobalAccel::self()->removeAllShortcuts(a);
                delete a;
            }
            shortcuts.clear();
            for (const auto &id : shortcutActions) {
                QString sequence = config[id].toObject()["sequence"].toString();
                if (sequence.trimmed().isEmpty() || modifierOnly(sequence))
                    continue;
                auto key = QKeySequence::fromString(sequence, QKeySequence::PortableText);
                if (shortcutConflict(key)) {
                    setStatus(
                        UiText::tr("Shortcut %1 is in use. Change it in Settings.").arg(sequence));
                    continue;
                }
                auto *a = new QAction(QString(UiText::tr("Ranslator: %1")).arg(id), this);
                a->setObjectName(id);
                a->setProperty("componentName", "ranslator");
                a->setProperty("componentDisplayName", "Ranslator");
                a->setAutoRepeat(false);
                connect(a, &QAction::triggered, this, [this, id] {
                    if (ShortcutRecorder::active()) {
                        const auto keys = KGlobalAccel::self()->globalShortcut("ranslator", id);
                        dispatchShortcut(id, keys.size() == 1,
                                         keys.size() == 1
                                             ? keys.first().toString(QKeySequence::PortableText)
                                             : QString());
                    } else {
                        dispatchShortcut(id);
                    }
                });
                KGlobalAccel::self()->setShortcut(a, {key}, KGlobalAccel::NoAutoloading);
                shortcuts.append(a);
            }
            return;
        }
#endif
        if (x11Shortcuts.available()) {
            x11Shortcuts.bind(
                config, [this](QString id) { dispatchShortcut(id); },
                [this](QString error) { setStatus(error); });
            return;
        }
#ifdef Q_OS_LINUX
        if (gnomeDesktop.available()) {
            portal.cancelRequests();
            gnomeDesktop.bind(
                config,
                [this](QString id, QJsonObject context) {
                    dispatchShortcut(id, true, {}, context);
                },
                [this](QString error) { setStatus(error); });
            return;
        }
        portal.bindShortcuts(
            config, [this](QString id) { dispatchShortcut(id, false); },
            [this](QString error) { setStatus(error); });
#endif
    }
    void openPacks(QLineEdit *codes, QLineEdit *directory, QDialog *parent) {
        auto *dialog = new GlassDialog(parent);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(UiText::tr("OCR language packs"));
        dialog->setWindowModality(Qt::WindowModal);
        dialog->resize(600, 640);
        auto *layout = new QVBoxLayout(dialog);
        layout->setContentsMargins(42, 42, 42, 42);
        layout->addWidget(label(UiText::tr("Read more languages"), "heading"));
        auto *explanation = label(
            UiText::tr(
                "Choose up to eight languages you expect to see. Fewer languages give faster, more "
                "accurate recognition. Downloads come from Tesseract's official model repository."),
            "muted");
        explanation->setWordWrap(true);
        layout->addWidget(explanation);
        auto *list = new QScrollArea;
        list->setWidgetResizable(true);
        auto *listContents = new QWidget;
        auto *listLayout = new QVBoxLayout(listContents);
        listLayout->setAlignment(Qt::AlignTop);
        list->setWidget(listContents);
        const auto checks = std::make_shared<QList<QCheckBox *>>();
        list->setAccessibleName(UiText::tr("OCR languages to install"));
        layout->addWidget(list);
        auto *bulk = button(UiText::tr("Download all packs"));
        auto *selectAll = button(UiText::tr("Select all"));
        auto *clearChoices = button(UiText::tr("Clear selection"));
        auto *bulkRow = new QHBoxLayout;
        bulkRow->addWidget(bulk);
        bulkRow->addWidget(selectAll);
        bulkRow->addWidget(clearChoices);
        layout->addLayout(bulkRow);
        auto *message = label(UiText::tr("Loading language packs…"), "muted");
        message->setWordWrap(true);
        layout->addWidget(message);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
        auto *apply = button(UiText::tr("Use selected"), "primary");
        buttons->addButton(apply, QDialogButtonBox::AcceptRole);
        apply->setEnabled(false);
        layout->addWidget(buttons);
        const auto id = ++settingsRequest;
        const auto downloadAll = std::make_shared<bool>(false);
        const QPointer<QDialog> guard(dialog);
        const auto chosen = codes->text().split('+');
        auto update = [=] {
            int count = 0;
            qint64 bytes = 0;
            for (auto *check : *checks) {
                if (check->isChecked()) {
                    ++count;
                    bytes += check->property("bytes").toLongLong();
                }
            }
            apply->setEnabled(count > 0 && (count <= 8 || bytes > 0));
            apply->setText(count > 8   ? UiText::tr("Download selected")
                           : bytes > 0 ? UiText::tr("Download and use")
                                       : UiText::tr("Use selected"));
            message->setText(QString(UiText::tr("%1 selected · %2 MB to download"))
                                 .arg(count)
                                 .arg(bytes / 1000000.0, 0, 'f', 1));
        };
        onPacks = [=](QJsonObject value) {
            if (!guard || quint64(value["id"].toDouble()) != id)
                return;
            const auto type = value["type"].toString();
            if (type == "packs_list") {
                bulk->setEnabled(true);
                qDeleteAll(*checks);
                checks->clear();
                for (auto entry : value["packs"].toArray()) {
                    auto pack = entry.toObject();
                    const auto code = pack["code"].toString();
                    const bool installed = pack["installed"].toBool();
                    auto *check = new QCheckBox(
                        pack["name"].toString() +
                            (installed ? UiText::tr(" · Ready")
                                       : QString(UiText::tr(" · %1 MB"))
                                             .arg(pack["bytes"].toDouble() / 1000000.0, 0, 'f', 1)),
                        listContents);
                    check->setMinimumHeight(30);
                    check->setProperty("code", code);
                    check->setProperty("bytes", installed ? 0 : pack["bytes"].toDouble());
                    listLayout->addWidget(check);
                    checks->append(check);
                    check->setChecked(chosen.contains(code));
                    connect(check, &QCheckBox::toggled, dialog, [update](bool) { update(); });
                }
                update();
            } else if (type == "packs_progress")
                message->setText(value["text"].toString());
            else if (type == "packs_done") {
                clearChoices->setEnabled(true);
                selectAll->setEnabled(true);
                bulk->setEnabled(true);
                const auto error = value["error"].toString();
                if (!error.isEmpty()) {
                    list->setEnabled(true);
                    bulk->setEnabled(true);
                    *downloadAll = false;
                    update();
                    message->setText(error);
                } else if (*downloadAll) {
                    *downloadAll = false;
                    list->setEnabled(true);
                    message->setText(UiText::tr(
                        "All packs downloaded. Choose your active OCR languages below."));
                    send({{"type", "list_packs"},
                          {"id", qint64(id)},
                          {"directory", directory->text()}});
                } else {
                    QStringList selected;
                    for (auto *check : *checks)
                        if (check->isChecked())
                            selected << check->property("code").toString();
                    codes->setText(selected.join('+'));
                    dialog->accept();
                }
            }
        };
        connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
        connect(dialog, &QDialog::finished, this, [](int) { send({{"type", "cancel_packs"}}); });
        connect(selectAll, &QPushButton::clicked, dialog, [=] {
            for (auto *check : *checks)
                check->setChecked(true);
        });
        connect(clearChoices, &QPushButton::clicked, dialog, [=] {
            for (auto *check : *checks)
                check->setChecked(false);
        });
        connect(bulk, &QPushButton::clicked, dialog, [=] {
            QJsonArray missing;
            for (auto *check : *checks)
                if (check->property("bytes").toLongLong() > 0)
                    missing.append(check->property("code").toString());
            if (missing.isEmpty()) {
                message->setText(UiText::tr("All language packs are already installed."));
                return;
            }
            *downloadAll = true;
            list->setEnabled(false);
            apply->setEnabled(false);
            bulk->setEnabled(false);
            clearChoices->setEnabled(false);
            selectAll->setEnabled(false);
            send({{"type", "install_packs"}, {"id", qint64(id)}, {"codes", missing}});
        });
        connect(apply, &QPushButton::clicked, dialog, [=] {
            QJsonArray selected;
            QJsonArray download;
            bool missing = false;
            for (auto *check : *checks)
                if (check->isChecked()) {
                    selected.append(check->property("code").toString());
                    missing |= check->property("bytes").toLongLong() > 0;
                    if (check->property("bytes").toLongLong() > 0)
                        download.append(check->property("code").toString());
                }
            if (selected.size() > 8)
                *downloadAll = true;
            if (!missing) {
                QStringList names;
                for (auto value : selected)
                    names << value.toString();
                codes->setText(names.join('+'));
                dialog->accept();
                return;
            }
            list->setEnabled(false);
            apply->setEnabled(false);
            bulk->setEnabled(false);
            selectAll->setEnabled(false);
            message->setText(UiText::tr("Starting download…"));
            send({{"type", "install_packs"}, {"id", qint64(id)}, {"codes", download}});
        });
        send({{"type", "list_packs"}, {"id", qint64(id)}, {"directory", directory->text()}});
        dialog->show();
    }
    void openSettings() {
        autostart->refresh();
        if (settings) {
            settings->show();
            settings->raise();
            return;
        }
        auto *dialog = new GlassDialog(this);
        settings = dialog;
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(UiText::tr("Ranslator Settings"));
        dialog->resize(850, 690);
        dialog->setMinimumSize(740, 580);
        auto *layout = new QVBoxLayout(dialog);
        layout->setContentsMargins(42, 36, 42, 36);
        layout->setSpacing(16);
        auto *heading = new QHBoxLayout;
        heading->addWidget(label("Ranslator", "brand"));
        heading->addWidget(label(UiText::tr("/ Settings"), "muted"));
        heading->addStretch();
        auto *close = iconButton("close", UiText::tr("Close settings"));
        connect(close, &QPushButton::clicked, dialog, &QDialog::reject);
        heading->addWidget(close);
        layout->addLayout(heading);
        auto *body = new QHBoxLayout;
        body->setSpacing(24);
        auto *sidebar = new GlassSurface;
        sidebar->setFixedWidth(166);
        auto *navigation = new QVBoxLayout(sidebar);
        navigation->setContentsMargins(9, 12, 9, 12);
        navigation->setSpacing(6);
        auto *tabs = new QStackedWidget;
        tabs->setObjectName("settingsPages");
        auto *group = new QButtonGroup(dialog);
        group->setExclusive(true);
        body->addWidget(sidebar);
        body->addWidget(tabs, 1);
        layout->addLayout(body, 1);
        auto page = [=](QString title, QString subtitle = {}) {
            auto *w = new QWidget;
            auto *f = new SettingsForm(w);
            f->setContentsMargins(0, 10, 16, 20);
            f->setVerticalSpacing(8);
            f->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

            f->addRow(label(title, "heading"));
            if (!subtitle.isEmpty()) {
                auto *help = label(subtitle, "muted");
                help->setWordWrap(true);
                f->addRow(help);
            }
            auto *scroll = new QScrollArea;
            scroll->setWidgetResizable(true);
            scroll->setFrameShape(QFrame::NoFrame);
            scroll->setWidget(w);
            const int index = tabs->addWidget(scroll);
            auto *nav = button(title);
            nav->setProperty("navigation", true);
            nav->setProperty("quiet", true);
            nav->setProperty(
                "glyph",
                QStringList{"language", "spark", "keyboard", "scan", "appearance", "loop"}.value(
                    index));
            nav->setMinimumHeight(42);
            nav->setCheckable(true);
            group->addButton(nav, index);
            navigation->addWidget(nav);
            if (index == 0)
                nav->setChecked(true);
            connect(nav, &QPushButton::clicked, dialog, [=] {
                tabs->setCurrentIndex(index);
                Design::fadeIn(scroll);
            });
            return f;
        };
        auto *general = page(UiText::tr("Languages"),
                             UiText::tr("One preferred language. A natural way back."));
        auto *preferred = languages(false), *fallback = languages(false);
        preferred->removeItem(0);
        fallback->removeItem(0);
        setLanguage(preferred, config["preferred_language"].toString());
        setLanguage(fallback, config["fallback_language"].toString());
        general->addRow(UiText::tr("Translate into"), preferred);
        general->addRow(UiText::tr("When text is already in that language"), fallback);
        auto *popupMode = new QCheckBox(UiText::tr("Show translations as a compact popup"));
        popupMode->setChecked(config["popup_mode"].toBool(true));
        general->addRow(popupMode);
        auto *popupHelp =
            label(UiText::tr("Appears only when translating. Click outside to dismiss; expand "
                             "to keep working in the full app."),
                  "muted");
        popupHelp->setWordWrap(true);
        general->addRow(popupHelp);
        auto *explain = label(
            UiText::tr(
                "Your preferred language is used automatically. Text detected in that language "
                "uses your fallback instead. You can choose any target on the translation card."),
            "muted");
        explain->setWordWrap(true);
        general->addRow(explain);
        auto *providers =
            page(UiText::tr("Translation"), UiText::tr("Choose the engine that understands you."));
        auto *provider = new GlassComboBox;
        provider->setObjectName("providerSelect");
        for (auto item : QList<QPair<QString, QString>>{
                 {UiText::tr("Automatic · local Ollama, otherwise MyMemory"), "auto"},
                 {UiText::tr("Ollama · local models"), "ollama"},
                 {UiText::tr("Compatible LLM API"), "compatible"},
                 {"LibreTranslate", "libretranslate"},
                 {UiText::tr("MyMemory · free online service"), "mymemory"},
                 {UiText::tr("Local command / runtime"), "cli"}})
            provider->addItem(item.first, item.second);
        provider->setCurrentIndex(provider->findData(config["provider"].toString()));
        providers->addRow(UiText::tr("Provider"), provider);
        auto *endpoint = new QLineEdit(config["endpoint"].toString());
        endpoint->setObjectName("providerEndpoint");
        endpoint->setPlaceholderText(UiText::tr("http://127.0.0.1:11434 or https://server/v1"));
        providers->addRow(UiText::tr("Base URL"), endpoint);
        auto *model = new GlassComboBox;
        model->setObjectName("providerModel");
        model->setEditable(true);
        model->setInsertPolicy(QComboBox::NoInsert);
        model->setAccessibleName(UiText::tr("Translation model"));
        model->setEditText(config["model"].toString());
        model->lineEdit()->setPlaceholderText(
            UiText::tr("First installed model, or type a model name"));
        auto *modelRow = new QWidget;
        auto *modelLayout = new QHBoxLayout(modelRow);
        modelLayout->setContentsMargins(0, 0, 0, 0);
        auto *refresh = button(UiText::tr("Refresh"));
        refresh->setAccessibleName(UiText::tr("Refresh available models"));
        modelLayout->addWidget(model, 1);
        modelLayout->addWidget(refresh);
        providers->addRow(UiText::tr("Model"), modelRow);
        auto *modelStatus =
            label(UiText::tr("Discover models from your running local runtime or API."), "muted");
        modelStatus->setWordWrap(true);
        providers->addRow(modelStatus);
        auto *key = new QLineEdit(config["api_key"].toString());
        key->setObjectName("providerKey");
        key->setEchoMode(QLineEdit::Password);
        providers->addRow(UiText::tr("API key"), key);
        auto *keyEnv = new QLineEdit(config["api_key_env"].toString());
        keyEnv->setObjectName("providerKeyEnv");
        providers->addRow(UiText::tr("Or environment variable"), keyEnv);
        auto *runtime = new GlassComboBox;
        runtime->setAccessibleName(UiText::tr("CLI runtime"));
        runtime->setObjectName("runtimeSelect");
        runtime->addItem(UiText::tr("Custom command"), "custom");
        const auto initialRuntime = config["cli_runtime"].toString("custom");
        if (initialRuntime != "custom") {
            runtime->addItem(initialRuntime, initialRuntime);
            runtime->setCurrentIndex(1);
        }
        auto *detect = button(UiText::tr("Detect runtimes"));
        auto *runtimeRow = new QWidget;
        auto *runtimeLayout = new QHBoxLayout(runtimeRow);
        runtimeLayout->setContentsMargins(0, 0, 0, 0);
        runtimeLayout->setSpacing(10);
        runtimeLayout->addWidget(runtime, 1);
        runtimeLayout->addWidget(detect);
        providers->addRow(UiText::tr("CLI runtime"), runtimeRow);
        auto *freeOnly = new QCheckBox(UiText::tr("Show models labeled free by OpenCode"));
        freeOnly->setChecked(config["cli_free_only"].toBool(true));
        providers->addRow(freeOnly);
        auto *runtimeStatus = label("", "muted");
        runtimeStatus->setWordWrap(true);
        providers->addRow(runtimeStatus);
        auto *cli = new QLineEdit(config["cli_program"].toString());
        cli->setObjectName("runtimeExecutable");
        cli->setPlaceholderText("/path/to/translator");
        providers->addRow(UiText::tr("CLI executable"), cli);
        auto *args = new QLineEdit(QString::fromUtf8(
            QJsonDocument(config["cli_args"].toArray()).toJson(QJsonDocument::Compact)));
        args->setObjectName("runtimeArguments");
        args->setPlaceholderText("[\"--target\", \"{target}\"]");
        providers->addRow(UiText::tr("CLI arguments (JSON array)"), args);
        auto *timeout = new NumberField;
        timeout->setObjectName("requestTimeout");
        timeout->setRange(5, 300);
        timeout->setSuffix(UiText::tr(" seconds"));
        timeout->setValue(config["timeout_seconds"].toInt());
        providers->addRow(UiText::tr("Request timeout"), timeout);
        auto *privacy = label("", "muted");
        privacy->setWordWrap(true);
        providers->addRow(privacy);
        const auto request = std::make_shared<quint64>(0);
        const auto loading = std::make_shared<bool>(false);
        const auto restoring = std::make_shared<bool>(false);
        const auto modelEdited = std::make_shared<bool>(false);
        const auto profiles = std::make_shared<QJsonObject>(config["provider_profiles"].toObject());
        auto profileKey = [=] {
            const auto id = provider->currentData().toString();
            return id == "cli" ? id + "/" + runtime->currentData().toString() : id;
        };
        const auto activeProfile = std::make_shared<QString>(profileKey());
        auto readProfile = [=] {
            QJsonParseError parse;
            const auto document = QJsonDocument::fromJson(args->text().toUtf8(), &parse);
            return QJsonObject{
                {"endpoint", endpoint->text().trimmed()},
                {"model", model->currentText().trimmed()},
                {"api_key", key->text()},
                {"api_key_env", keyEnv->text().trimmed()},
                {"cli_program", cli->text().trimmed()},
                // Retain unfinished argument edits in the dialog; Save validates every draft.
                {"cli_args", parse.error == QJsonParseError::NoError && document.isArray()
                                 ? QJsonValue(document.array())
                                 : QJsonValue(args->text())},
                {"cli_free_only", freeOnly->isChecked()},
                {"timeout_seconds", timeout->value()}};
        };
        profiles->insert(*activeProfile, readProfile());
        auto canListModels = [=] {
            const auto id = provider->currentData().toString();
            return id == "auto" || id == "ollama" || id == "compatible" ||
                   (id == "cli" && runtime->currentData().toString() == "opencode");
        };
        const QPointer<QDialog> guard(dialog);
        auto refreshModels = [=, this] {
            if (!canListModels())
                return;
            auto candidate = config;
            const auto draft = readProfile();
            for (auto it = draft.begin(); it != draft.end(); ++it)
                candidate[it.key()] = it.value();
            candidate["provider_profiles"] = QJsonObject{};
            candidate["provider"] = provider->currentData().toString();
            candidate["cli_runtime"] = runtime->currentData().toString();
            *request = ++settingsRequest;
            *loading = true;
            *modelEdited = false;
            refresh->setEnabled(false);
            modelStatus->setText(UiText::tr("Looking for models…"));
            send({{"type", "list_models"}, {"id", qint64(*request)}, {"config", candidate}});
        };
        onModels = [=](QJsonObject value) {
            if (!guard || quint64(value["id"].toDouble()) != *request)
                return;
            *loading = false;
            refresh->setEnabled(canListModels());
            const auto error = value["error"].toString();
            if (!error.isEmpty()) {
                modelStatus->setText(error);
                return;
            }
            const auto selected = model->currentText();
            model->clear();
            for (auto entry : value["models"].toArray())
                model->addItem(entry.toString());
            // Compatible APIs can accept custom IDs absent from their catalog. Local catalogs
            // are authoritative, unless the user explicitly typed an ID during this lookup.
            if (!selected.isEmpty() && (model->findText(selected) >= 0 || *modelEdited ||
                                        provider->currentData().toString() == "compatible"))
                model->setEditText(selected);
            modelStatus->setText(
                model->count() > 0
                    ? UiText::tr("Connected · %n models available", nullptr, model->count())
                    : UiText::tr("Connected, but no models installed. Install a model in your "
                                 "runtime, then Refresh."));
        };
        connect(refresh, &QPushButton::clicked, dialog, refreshModels);
        auto updateProvider = [=] {
            const auto id = provider->currentData().toString();
            endpoint->setPlaceholderText(id == "ollama"
                                             ? UiText::tr("http://127.0.0.1:11434 (default)")
                                             : "https://server/v1");
            endpoint->setEnabled(id == "ollama" || id == "compatible" || id == "libretranslate");
            model->setEnabled(id == "auto" || id == "ollama" || id == "compatible" ||
                              (id == "cli" && runtime->currentData().toString() != "custom"));
            refresh->setEnabled(canListModels() && !*loading);
            key->setEnabled(id == "compatible" || id == "libretranslate");
            keyEnv->setEnabled(key->isEnabled());
            cli->setEnabled(id == "cli");
            args->setEnabled(id == "cli");
            providers->setRowVisible(runtimeRow, id == "cli");
            providers->setRowVisible(runtimeStatus, id == "cli");
            providers->setRowVisible(freeOnly, id == "cli" &&
                                                   runtime->currentData().toString() == "opencode");
            providers->setRowVisible(endpoint, endpoint->isEnabled());
            providers->setRowVisible(modelRow, model->isEnabled());
            providers->setRowVisible(modelStatus, model->isEnabled());
            providers->setRowVisible(key, key->isEnabled());
            providers->setRowVisible(keyEnv, keyEnv->isEnabled());
            providers->setRowVisible(cli, cli->isEnabled());
            providers->setRowVisible(args, args->isEnabled());
            providers->setRowVisible(timeout, id != "auto" && id != "mymemory");
            privacy->setText(
                id == "auto"
                    ? UiText::tr(
                          "Uses a local Ollama model when available. Otherwise, selected text "
                          "is sent to MyMemory online.")
                : id == "ollama"
                    ? UiText::tr("Models run on your Ollama server. Use localhost to keep your "
                                 "words on this device.")
                : id == "cli"
                    ? UiText::tr(
                          "Custom: raw text on stdin, translation on stdout. Presets add a "
                          "translation "
                          "prompt and decode runtime output. CLI models may use cloud services and "
                          "account quotas. Arguments remain editable: {target}, {source}, {model}.")
                    : UiText::tr("Only text you choose to translate is sent to this provider. "
                                 "Screenshots are processed on your device."));
        };
        auto invalidateModels = [=, this] {
            *request = ++settingsRequest;
            *loading = false;
            const auto selected = model->currentText();
            model->clear();
            model->setEditText(selected);
            refresh->setEnabled(canListModels());
            modelStatus->setText(
                canListModels() ? UiText::tr("Refresh to discover models for these settings.")
                                : UiText::tr("Uses the runtime default, or enter a model ID."));
        };
        auto switchProfile = [=] {
            if (*activeProfile == profileKey())
                return;
            profiles->insert(*activeProfile, readProfile());
            *activeProfile = profileKey();
            const bool known = profiles->contains(*activeProfile);
            auto draft = profiles->value(*activeProfile).toObject();
            if (!known && provider->currentData().toString() == "cli" &&
                runtime->currentData().toString() != "custom") {
                const auto id = runtime->currentData().toString();
                auto path = runtime->currentData(Qt::UserRole + 1).toJsonValue().toString();
                if (path.isEmpty())
                    path = id == "antigravity" ? "agy" : id;
                draft["cli_program"] = path;
                draft["cli_args"] = runtime->currentData(Qt::UserRole + 2).toJsonValue().toArray();
            }
            *restoring = true;
            endpoint->setText(draft["endpoint"].toString());
            model->clear();
            model->setEditText(draft["model"].toString());
            key->setText(draft["api_key"].toString());
            keyEnv->setText(draft["api_key_env"].toString("RANSLATOR_API_KEY"));
            cli->setText(draft["cli_program"].toString());
            args->setText(draft["cli_args"].isString()
                              ? draft["cli_args"].toString()
                              : QString::fromUtf8(QJsonDocument(draft["cli_args"].toArray())
                                                      .toJson(QJsonDocument::Compact)));
            freeOnly->setChecked(draft["cli_free_only"].toBool(true));
            timeout->setValue(draft["timeout_seconds"].toInt(90));
            *restoring = false;
            invalidateModels();
            updateProvider();
            refreshModels();
        };
        connect(provider, &QComboBox::currentIndexChanged, dialog, [=](int) { switchProfile(); });
        connect(runtime, &QComboBox::currentIndexChanged, dialog, [=](int) { switchProfile(); });
        connect(model->lineEdit(), &QLineEdit::textEdited, dialog, [=] { *modelEdited = true; });
        connect(model, &QComboBox::activated, dialog, [=] { *modelEdited = true; });
        for (auto *field : {endpoint, key, keyEnv, cli, args}) {
            connect(field, &QLineEdit::textChanged, dialog, [=] {
                if (!*restoring)
                    invalidateModels();
            });
            connect(field, &QLineEdit::editingFinished, dialog, [=] {
                if (!*restoring && !*loading && canListModels())
                    refreshModels();
            });
        }
        updateProvider();
        connect(tabs, &QStackedWidget::currentChanged, dialog, [=](int index) {
            if (index == 1 && canListModels() && !*loading && model->count() == 0)
                refreshModels();
        });
        onRuntimes = [=](QJsonObject value) {
            if (!guard)
                return;
            const QSignalBlocker blocker(runtime);
            const auto selected = runtime->currentData().toString();
            runtime->clear();
            runtime->addItem(UiText::tr("Custom command"), "custom");
            int found = 0;
            for (auto entry : value["runtimes"].toArray()) {
                const auto item = entry.toObject();
                const bool installed = item["installed"].toBool();
                found += installed;
                runtime->addItem(item["name"].toString() + (installed
                                                                ? UiText::tr(" · Found")
                                                                : UiText::tr(" · Not installed")),
                                 item["id"].toString());
                runtime->setItemData(runtime->count() - 1, item["path"], Qt::UserRole + 1);
                runtime->setItemData(runtime->count() - 1, item["args"], Qt::UserRole + 2);
            }
            if (runtime->findData(selected) < 0)
                runtime->addItem(selected, selected);
            runtime->setCurrentIndex(runtime->findData(selected));
            runtimeStatus->setText(
                !value["host_error"].toString().isEmpty() ? value["host_error"].toString()
                : value["sandboxed"].toBool()
                    ? UiText::tr("%n runtimes found on the host. Uses each runtime's existing "
                                 "login; no account setup is changed.",
                                 nullptr, found)
                    : UiText::tr("%n runtimes found. Uses each runtime's existing login; no "
                                 "account setup is changed.",
                                 nullptr, found));
            updateProvider();
        };
        connect(detect, &QPushButton::clicked, dialog, [] { send({{"type", "list_runtimes"}}); });
        connect(freeOnly, &QCheckBox::toggled, dialog, [=](bool) {
            if (!*restoring && provider->currentData().toString() == "cli" &&
                runtime->currentData().toString() == "opencode") {
                invalidateModels();
                refreshModels();
            }
        });
        send({{"type", "list_runtimes"}});
        auto *bindings = page(UiText::tr("Shortcuts"),
                              UiText::tr("Stay in your flow. Call Ranslator with a gesture."));
        QList<ShortcutRecorder *> sequences;
        QList<QSpinBox *> taps;
        for (const auto &id : shortcutActions) {
            auto b = config[id].toObject();
            const QString title = id == "translate_selection" ? UiText::tr("Translate selection")
                                  : id == "once"              ? UiText::tr("Select once")
                                  : id == "continuous"        ? UiText::tr("Toggle continuous")
                                                              : UiText::tr("Snapshot");
            auto *row = new QWidget;
            auto *h = new QHBoxLayout(row);
            h->setContentsMargins(0, 0, 0, 0);
            auto *seq = new ShortcutRecorder(b["sequence"].toString(), [](bool active) {
                send({{"type", "shortcut_recording"}, {"active", active}});
            });
            seq->setObjectName(QString("shortcut_%1").arg(id));
            seq->setAccessibleName(QString(UiText::tr("Record shortcut: %1")).arg(title));
            auto *tap = new NumberField;
            tap->setObjectName(QString("shortcutTaps_%1").arg(id));
            tap->setRange(1, 3);
            tap->setValue(b["taps"].toInt());
            const auto describePresses = [tap](int count) {
                // The number is drawn by QSpinBox; n still selects the language's plural form.
                tap->setSuffix(count == 1 ? UiText::tr(" press", nullptr, count)
                                          : UiText::tr(" presses", nullptr, count));
            };
            describePresses(tap->value());
            connect(tap, &QSpinBox::valueChanged, tap, describePresses);
            tap->setAccessibleName(QString(UiText::tr("Shortcut presses: %1")).arg(title));
            h->addWidget(seq, 1);
            h->addWidget(tap);
            if (id == "translate_selection") {
                // Optional: the other actions remain available without it.
                auto *off = button(UiText::tr("Turn off"));
                off->setObjectName("shortcutClear_translate_selection");
                off->setAccessibleName(UiText::tr("Turn off shortcut: %1").arg(title));
                connect(off, &QPushButton::clicked, seq, [seq] { seq->clear(); });
                h->addWidget(off);
            }
            sequences << seq;
            taps << tap;
            bindings->addRow(title, row);
        }
        auto *tapWindow = new NumberField;
        tapWindow->setRange(150, 2000);
        tapWindow->setSuffix(UiText::tr(" ms"));
        tapWindow->setValue(config["tap_window_ms"].toInt());
        bindings->addRow(UiText::tr("Time between presses"), tapWindow);
        auto *settle = new NumberField;
        settle->setRange(100, 3000);
        settle->setSuffix(UiText::tr(" ms"));
        settle->setValue(config["selection_delay_ms"].toInt());
        bindings->addRow(UiText::tr("Wait for selection to settle"), settle);
        auto *clipboard = new QCheckBox(UiText::tr("Also translate copied text while listening"));
        clipboard->setChecked(config["watch_clipboard"].toBool());
        bindings->addRow(clipboard);
        auto *rules = label(
            UiText::tr(
                "Choose how selected text gets translated. Translate selection: select text "
                "first, then press the shortcut. Select once: press the shortcut, then select. "
                "Toggle continuous: every new selection is translated until you press it again."),
            "muted");
        rules->setObjectName("shortcutRules");
        rules->setWordWrap(true);
        bindings->insertRow(2, rules); // After the page heading and subtitle.
        auto *help = label(
            UiText::tr(
                "Click a shortcut to record it, then press and release the keys. Esc cancels; "
                "click outside to keep the previous shortcut. Save applies your changes. "
                "Choose 1–3 presses separately. Examples: Ctrl+Alt+R, Shift ×2, Ctrl+Shift ×2. "
                "Modifier-only chords need support "
                "from your desktop and count on release. A selection triggers after it stays "
                "unchanged for the delay above. The same shortcut toggles each listener off."),
            "muted");
        help->setWordWrap(true);
        bindings->addRow(help);
#ifdef Q_OS_MACOS
        auto *copyHelp = label(
            UiText::tr("Translate selection reads the selection through Accessibility. In apps "
                       "that do not share it, Ranslator copies it with ⌘C and then restores your "
                       "clipboard. Password fields are never read."),
            "muted");
        copyHelp->setWordWrap(true);
        bindings->addRow(copyHelp);
#endif
        auto *ocr = page(UiText::tr("Screen text"),
                         UiText::tr("Turn the words you see into words you can select."));
        auto *copySnapshot = new QCheckBox(UiText::tr("Copy selected snapshot text to clipboard"));
        copySnapshot->setChecked(config["snapshot_copy_text"].toBool());
        ocr->addRow(copySnapshot);
        auto *captureMode = new GlassComboBox;
        captureMode->setObjectName("captureMode");
        captureMode->setAccessibleName(UiText::tr("Snapshot capture mode"));
        captureMode->addItem(UiText::tr("Current monitor · where Snapshot is invoked"), "auto");
        captureMode->addItem(UiText::tr("All monitors · combined overview"), "desktop");
        captureMode->addItem(UiText::tr("Select a rectangular area"), "region");
#ifndef Q_OS_MACOS
        captureMode->addItem(UiText::tr("System screenshot picker"), "picker");
#endif
        captureMode->setCurrentIndex(
            captureMode->findData(config["capture_mode"].toString("auto")));
        if (captureMode->currentIndex() < 0)
            captureMode->setCurrentIndex(captureMode->findData("region"));
        ocr->addRow(UiText::tr("Snapshot"), captureMode);
        auto *captureHelp = label("", "muted");
        captureHelp->setObjectName("captureModeHelp");
        captureHelp->setWordWrap(true);
        auto describeCapture = [this, captureMode, captureHelp] {
            const auto mode = captureMode->currentData().toString();
            const bool nativeCapture = Capture::directCaptureAvailable() ||
                                       Capture::x11Available() || gnomeDesktop.available()
#ifdef Q_OS_MACOS
                                       || true
#endif
                ;
            if (mode == "picker" || !nativeCapture) {
                captureHelp->setText(UiText::tr(
                    "Your desktop's screenshot service chooses the monitor or area and may ask "
                    "for permission. Available capture presets depend on that service. The "
                    "selection view returns to the monitor where Snapshot was invoked."));
            } else if (mode == "desktop") {
                captureHelp->setText(UiText::tr(
                    "Captures every connected monitor as one image. The combined overview opens "
                    "on the monitor where you invoked Snapshot; scroll to zoom into it."));
            } else if (mode == "region") {
                captureHelp->setText(UiText::tr(
                    "Choose a rectangular area, then select its text in the frozen snapshot."));
            } else {
                captureHelp->setText(UiText::tr(
                    "Captures the current monitor. The frozen selection view and translation "
                    "stay on that monitor. Move to the desired monitor before pressing the "
                    "shortcut."));
            }
        };
        connect(captureMode, &QComboBox::currentIndexChanged, dialog, describeCapture);
        describeCapture();
        ocr->addRow(captureHelp);
        // Apple Vision exists only on macOS. Elsewhere the saved value is kept and Tesseract runs.
        auto *ocrEngine = new GlassComboBox;
        ocrEngine->setObjectName("ocrEngine");
        ocrEngine->setAccessibleName(UiText::tr("Text recognition engine"));
#ifdef Q_OS_MACOS
        ocrEngine->addItem(UiText::tr("Apple Vision · built into macOS"), "vision");
#endif
        ocrEngine->addItem(UiText::tr("Tesseract · uses language packs"), "tesseract");
        ocrEngine->setCurrentIndex(std::max(
            0, ocrEngine->findData(
                   config["ocr_engine"].toString("auto") == "tesseract" ? "tesseract" : "vision")));
        ocr->addRow(UiText::tr("Recognition"), ocrEngine);
        ocr->setRowVisible(ocrEngine, ocrEngine->count() > 1);
        auto *ocrLang = new QLineEdit(config["ocr_languages"].toString());
        ocrLang->setObjectName("ocrLanguages");
        ocr->addRow(UiText::tr("OCR languages"), ocrLang);
        auto *data = new QLineEdit(config["tessdata_dir"].toString());
        data->setObjectName("ocrModelDirectory");
        data->setPlaceholderText(UiText::tr("Bundled English model"));
        ocr->addRow(UiText::tr("Custom model directory"), data);
        auto *packs = button(UiText::tr("Language packs…"));
        packs->setObjectName("ocrLanguagePacks");
        ocr->addRow(packs);
        connect(packs, &QPushButton::clicked, dialog,
                [=, this] { openPacks(ocrLang, data, dialog); });
        auto *ocrHelp = label("", "muted");
        ocrHelp->setObjectName("ocrEngineHelp");
        ocrHelp->setWordWrap(true);
        ocr->addRow(ocrHelp);
        auto describeEngine = [ocrEngine, ocrLang, data, packs, ocrHelp] {
            const bool vision = ocrEngine->currentData().toString() == "vision";
            // Vision needs no model files, so Tesseract's model controls would have no effect.
            data->setEnabled(!vision);
            packs->setEnabled(!vision);
            ocrHelp->setText(
                vision ? UiText::tr(
                             "Apple Vision reads most scripts and detects languages on its own; "
                             "OCR languages tell it which to prefer. The first recognition after "
                             "installing or updating macOS prepares its models in the background "
                             "and can take about a minute. Screenshot text recognition stays on "
                             "your device; only the words you select go to the translation "
                             "provider.")
                       : UiText::tr(
                             "English is included. Language packs downloads extra OCR languages "
                             "in one step. You can also use custom Tesseract files with codes "
                             "such as eng+rus or eng+jpn. Screenshot text recognition stays on "
                             "your device; only the words you select go to the translation "
                             "provider."));
        };
        connect(ocrEngine, &QComboBox::currentIndexChanged, dialog, describeEngine);
        describeEngine();
        auto *appearance = page(UiText::tr("Appearance"),
                                UiText::tr("Glass, light, and a little room to breathe."));
        auto *theme = new GlassComboBox;
        theme->addItem(UiText::tr("Follow system"), "system");
        theme->addItem(UiText::tr("Light"), "light");
        theme->addItem(UiText::tr("Dark"), "dark");
        theme->setCurrentIndex(theme->findData(config["appearance"].toString("system")));
        appearance->addRow(UiText::tr("Color scheme"), theme);
        auto *opaque = new QCheckBox(UiText::tr("Reduce transparency"));
        opaque->setChecked(config["reduce_transparency"].toBool());
        auto *motion = new QCheckBox(UiText::tr("Reduce motion"));
        motion->setChecked(config["reduce_motion"].toBool());
        appearance->addRow(opaque);
        appearance->addRow(motion);
        auto *materialHelp =
            label(UiText::tr("Glass picks up the colors behind your window. Reduce "
                             "transparency for a solid, higher-contrast surface."),
                  "muted");
        materialHelp->setWordWrap(true);
        appearance->addRow(materialHelp);
        auto previewAppearance = [=, this] {
            auto candidate = config;
            candidate["appearance"] = theme->currentData().toString();
            candidate["reduce_transparency"] = opaque->isChecked();
            candidate["reduce_motion"] = motion->isChecked();
            Design::configure(candidate);
        };
        connect(theme, &QComboBox::currentIndexChanged, dialog, [=](int) { previewAppearance(); });
        connect(opaque, &QCheckBox::toggled, dialog, [=](bool) { previewAppearance(); });
        connect(motion, &QCheckBox::toggled, dialog, [=](bool) { previewAppearance(); });
        connect(dialog, &QDialog::finished, this, [this](int) { Design::configure(config); });
        auto *startup = page(UiText::tr("Startup"), UiText::tr("Ready when you log in."));
        autostartToggle = new QCheckBox(UiText::tr("Start Ranslator when I log in"));
        autostartToggle->setObjectName("autostartToggle");
        startup->addRow(autostartToggle);
        autostartDetail = label("", "muted");
        autostartDetail->setObjectName("autostartDetail");
        autostartDetail->setWordWrap(true);
        startup->addRow(autostartDetail);
        startup->addRow(label(UiText::tr("Changes take effect immediately."), "muted"));
        loginSettingsButton = button(UiText::tr("Open Login Items settings"));
        startup->addRow(loginSettingsButton);
        connect(loginSettingsButton, &QPushButton::clicked, dialog,
                [this] { autostart->openSystemSettings(); });
        connect(autostartToggle, &QCheckBox::toggled, dialog,
                [this](bool enabled) { changeAutostart(enabled); });
        syncAutostart();
        navigation->addStretch();
        auto *error = label("", "error");
        error->setWordWrap(true);
        settingsError = error;
        layout->addWidget(error);
        auto *buttons = new QDialogButtonBox;
        buttons->addButton(button(UiText::tr("Save"), "primary"), QDialogButtonBox::AcceptRole);
        buttons->addButton(button(UiText::tr("Close")), QDialogButtonBox::RejectRole);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
        connect(buttons, &QDialogButtonBox::accepted, dialog, [=, this] {
            error->setStyleSheet("");
            auto candidate = config;
            candidate["preferred_language"] = languageValue(preferred);
            candidate["popup_mode"] = popupMode->isChecked();
            candidate["fallback_language"] = languageValue(fallback);
            candidate["provider"] = provider->currentData().toString();
            candidate["endpoint"] = endpoint->text().trimmed();
            candidate["cli_program"] = cli->text().trimmed();
            candidate["cli_runtime"] = runtime->currentData().toString();
            candidate["cli_free_only"] = freeOnly->isChecked();
            candidate["model"] = model->currentText().trimmed();
            candidate["api_key"] = key->text();
            candidate["api_key_env"] = keyEnv->text().trimmed();
            candidate["cli_program"] = cli->text().trimmed();
            QJsonParseError parse;
            auto parsed = QJsonDocument::fromJson(args->text().toUtf8(), &parse);
            if (parse.error != QJsonParseError::NoError || !parsed.isArray()) {
                error->setText(UiText::tr("CLI arguments must be a JSON array of strings."));
                return;
            }
            for (auto value : parsed.array())
                if (!value.isString()) {
                    error->setText(UiText::tr("Every CLI argument must be a string."));
                    return;
                }
            candidate["cli_args"] = parsed.array();
            candidate["timeout_seconds"] = timeout->value();
            profiles->insert(*activeProfile, readProfile());
            for (auto it = profiles->begin(); it != profiles->end(); ++it) {
                const auto arguments = it.value().toObject()["cli_args"];
                bool valid = arguments.isArray();
                for (const auto &argument : arguments.toArray())
                    valid = valid && argument.isString();
                if (!valid) {
                    error->setText(UiText::tr("Fix the CLI argument array in %1 before saving.")
                                       .arg(it.key()));
                    return;
                }
            }
            candidate["provider_profiles"] = *profiles;
            candidate["tap_window_ms"] = tapWindow->value();
            candidate["selection_delay_ms"] = settle->value();
            candidate["watch_clipboard"] = clipboard->isChecked();
            candidate["ocr_languages"] = ocrLang->text().trimmed();
#ifdef Q_OS_MACOS
            candidate["ocr_engine"] = ocrEngine->currentData().toString();
#endif
            candidate["tessdata_dir"] = data->text().trimmed();
            candidate["snapshot_copy_text"] = copySnapshot->isChecked();
            candidate["capture_mode"] = captureMode->currentData().toString();
            candidate["appearance"] = theme->currentData().toString();
            candidate["reduce_transparency"] = opaque->isChecked();
            candidate["reduce_motion"] = motion->isChecked();
            int i = 0;
            for (const auto &id : shortcutActions) {
                candidate[id] =
                    QJsonObject{{"sequence", sequences[i]->sequence()}, {"taps", taps[i]->value()}};
                ++i;
            }
            auto problem = validateShortcuts(candidate);
            if (!problem.isEmpty()) {
                error->setText(problem);
                return;
            }
            send({{"type", "save_config"}, {"config", candidate}});
        });
        dialog->show();
    }

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (event->type() == QEvent::WindowDeactivate) {
            if (auto *recorder = ShortcutRecorder::active();
                recorder && watched == recorder->window())
                recorder->cancelOnWindowDeactivation();
        }
        if (event->type() == QEvent::Hide || event->type() == QEvent::Show ||
            event->type() == QEvent::WindowActivate || event->type() == QEvent::WindowDeactivate) {
            if (auto *widget = qobject_cast<QWidget *>(watched); widget && widget->isWindow())
                QTimer::singleShot(0, this, [this] { syncSelectionFocus(); });
        }
        return GlassWindow::eventFilter(watched, event);
    }
    void closeEvent(QCloseEvent *e) override {
        if (QSystemTrayIcon::isSystemTrayAvailable()) {
            hide();
            e->ignore();
        } else {
            send({{"type", "quit"}});
            qApp->quit();
        }
    }

  public:
    Window(QJsonObject cfg, bool smoke, Autostart *startup = nullptr)
        : config(cfg), portal(this), autostart(startup ? startup : new Autostart(this, !smoke)),
          x11Shortcuts(this), accessibleSelection(this), gnomeDesktop(this, !smoke),
          smokeMode(smoke) {
        setObjectName("card");
        setWindowTitle("Ranslator");
        setMinimumSize(530, 650);
        resize(564, 720);
        auto *layout = new QVBoxLayout(this);
        // The painted glass begins 20 px inside the window's shadow canvas.
        // Leave real padding inside that rim, including the rounded header corners.
        layout->setContentsMargins(44, 42, 44, 40);
        layout->setSpacing(12);
        auto *top = new QHBoxLayout;
        auto *title = new QVBoxLayout;
        title->setSpacing(4);
        title->addWidget(label("Ranslator", "brand"));
        title->addWidget(label(UiText::tr("A little closer to understanding."), "muted"));
        top->addLayout(title);
        top->addStretch();
        auto *settingsButton = iconButton("settings", UiText::tr("Open settings"));
        auto *closeButton = iconButton("close", UiText::tr("Close translator"));
        top->addWidget(settingsButton);
        top->addWidget(closeButton);
        connect(closeButton, &QPushButton::clicked, this, &QWidget::close);
        connect(settingsButton, &QPushButton::clicked, this, [this] { openSettings(); });
        layout->addLayout(top);
        layout->addSpacing(4);
        auto *languageSurface = new GlassSurface;
        languageSurface->setProperty("radius", 23);
        auto *langs = new QHBoxLayout(languageSurface);
        langs->setContentsMargins(10, 5, 10, 5);
        langs->setSpacing(0);
        sourceLanguage = languages(true);
        targetLanguage = languages(false);
        sourceLanguage->setObjectName("language");
        targetLanguage->setObjectName("language");
        sourceLanguage->setMinimumWidth(140);
        targetLanguage->setMinimumWidth(140);
        updateLanguageHint();
        langs->addWidget(sourceLanguage, 1);
        auto *swap = iconButton("swap", UiText::tr("Swap languages"));
        langs->addWidget(swap);
        langs->addWidget(targetLanguage, 1);
        connect(swap, &QPushButton::clicked, this, [this] {
            auto source = languageValue(sourceLanguage);
            auto target = languageValue(targetLanguage);
            if (!translated->toPlainText().isEmpty()) {
                source = translatedSource;
                target = translatedTarget;
                sourceText->setPlainText(translated->toPlainText());
            }
            if (target == "auto")
                target = config["preferred_language"].toString();
            setLanguage(sourceLanguage, target);
            setLanguage(targetLanguage,
                        source == "auto" ? config["fallback_language"].toString() : source);
            if (!sourceText->toPlainText().isEmpty())
                translate();
        });
        layout->addWidget(languageSurface);
        auto *sourceSurface = new GlassSurface;
        sourceSurface->setObjectName("sourceEditorSurface");
        auto *sourceLayout = new QVBoxLayout(sourceSurface);
        sourceLayout->setContentsMargins(18, 12, 18, 14);
        sourceLayout->setSpacing(8);
        auto *sourceHeader = new QHBoxLayout;
        sourceHeader->addWidget(label(UiText::tr("ORIGINAL"), "eyebrow"));
        sourceHeader->addStretch();
        auto *clear = iconButton("close", UiText::tr("Clear text"));
        clear->setFixedSize(24, 24);
        sourceHeader->addWidget(clear);
        sourceLayout->addLayout(sourceHeader);
        sourceText = new QPlainTextEdit;
        sourceText->setObjectName("sourceText");
        sourceText->setPlaceholderText(
            UiText::tr("Type or paste text here…\nCtrl+Enter to translate"));
        sourceText->setAccessibleName(UiText::tr("Text to translate"));
        sourceText->setMinimumHeight(96);
        sourceText->setMaximumHeight(160);
        sourceText->setFrameShape(QFrame::NoFrame);
        sourceLayout->addWidget(sourceText, 1);
        sourceSurface->setFocusPolicy(Qt::StrongFocus);
        sourceSurface->setFocusProxy(sourceText);
        sourceSurface->setCursor(Qt::IBeamCursor);
        layout->addWidget(sourceSurface);
        auto *resultSurface = new GlassSurface;
        auto *resultLayout = new QVBoxLayout(resultSurface);
        resultLayout->setContentsMargins(20, 14, 20, 18);
        resultLayout->setSpacing(10);
        auto *outHeader = new QHBoxLayout;
        route = label(UiText::tr("TRANSLATION"), "eyebrow");
        outHeader->addWidget(route);
        outHeader->addStretch();
        copyButton = iconButton("copy", UiText::tr("Copy"));
        copyButton->setEnabled(false);
        outHeader->addWidget(copyButton);
        resultLayout->addLayout(outHeader);
        translated = new QPlainTextEdit;
        translated->setObjectName("translation");
        translated->setReadOnly(true);
        translated->setPlaceholderText(UiText::tr("Your words,\nin another language."));
        translated->setAccessibleName(UiText::tr("Translated text"));
        translated->setMinimumHeight(118);
        translated->setFrameShape(QFrame::NoFrame);
        resultLayout->addWidget(translated, 1);
        connect(clear, &QPushButton::clicked, this, [this] {
            send({{"type", "cancel"}});
            sourceText->clear();
            translated->clear();
            copyButton->setEnabled(false);
            route->setText(UiText::tr("TRANSLATION"));
            sourceText->setFocus();
        });
        connect(copyButton, &QPushButton::clicked, this, [this] {
            QApplication::clipboard()->setText(translated->toPlainText());
            setStatus(UiText::tr("Translation copied."));
        });
        progress = new QProgressBar;
        progress->setRange(0, 0);
        progress->setTextVisible(false);
        progress->setFixedHeight(3);
        progress->hide();
        resultLayout->addWidget(progress);
        auto *bottom = new QHBoxLayout;
        providerLabel = label(UiText::tr("Connecting…"), "provider");
        providerLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        bottom->addWidget(providerLabel, 1);
        stopButton = iconButton("close", UiText::tr("Stop"));
        stopButton->setEnabled(false);
        translateButton = button(UiText::tr("Translate"), "primary");
        translateButton->setProperty("glyph", "arrow");
        translateButton->setMinimumWidth(132);
        bottom->addWidget(stopButton);
        bottom->addWidget(translateButton);
        resultLayout->addLayout(bottom);
        layout->addWidget(resultSurface, 1);
        connect(stopButton, &QPushButton::clicked, this, [] { send({{"type", "cancel"}}); });
        connect(translateButton, &QPushButton::clicked, this, [this] { translate(); });
        auto *modeSurface = new GlassSurface;
        modeSurface->setProperty("radius", 26);
        auto *modes = new QHBoxLayout(modeSurface);
        modes->setContentsMargins(5, 5, 5, 5);
        modes->setSpacing(3);
        onceButton = button(UiText::tr("Select"));
        onceButton->setAccessibleName(UiText::tr("Select text"));
        onceButton->setProperty("glyph", "select");
        onceButton->setCheckable(true);
        continuousButton = button(UiText::tr("Auto translate"));
        continuousButton->setAccessibleName(UiText::tr("Keep listening"));
        continuousButton->setProperty("glyph", "loop");
        continuousButton->setCheckable(true);
        auto *snapshot = button(UiText::tr("Screen"));
        snapshot->setAccessibleName(UiText::tr("Snapshot"));
        snapshot->setProperty("glyph", "scan");
        for (auto *b : {onceButton, continuousButton, snapshot}) {
            b->setProperty("quiet", true);
            modes->addWidget(b, 1);
        }
        onceButton->setToolTip(UiText::tr("Select text once · %1").arg(bindingDescription("once")));
        continuousButton->setToolTip(
            UiText::tr("Translate each new text selection in other apps; click again to stop · %1")
                .arg(bindingDescription("continuous")));
        snapshot->setToolTip(
            UiText::tr("Select text on screen · %1").arg(bindingDescription("snapshot")));
        connect(onceButton, &QPushButton::clicked, this, [] { action("once"); });
        connect(continuousButton, &QPushButton::clicked, this, [] { action("continuous"); });
        connect(snapshot, &QPushButton::clicked, this, [] { action("snapshot"); });
        layout->addWidget(modeSurface);
        status = label(readyHint(), "status");
        status->setAlignment(Qt::AlignCenter);
        status->setWordWrap(true);
        layout->addWidget(status);
        auto *shortcut = new QShortcut(QKeySequence("Ctrl+Return"), this);
        connect(shortcut, &QShortcut::activated, this, [this] { translate(); });
        auto *escape = new QShortcut(QKeySequence(Qt::Key_Escape), this);
        connect(escape, &QShortcut::activated, this, [this] { hide(); });
        connect(targetLanguage, &QComboBox::activated, this, [this](int) {
            if (!sourceText->toPlainText().isEmpty())
                translate();
        });
        connect(qApp, &QGuiApplication::applicationStateChanged, this,
                [this](Qt::ApplicationState) { syncSelectionFocus(); });
        connect(qApp, &QGuiApplication::focusWindowChanged, this,
                [this](QWindow *) { syncSelectionFocus(); });
        qApp->installEventFilter(this);
        connect(&accessibleSelection, &AccessibilitySelection::selected, this,
                [](const QString &text, quint64 generation) {
                    send({{"type", "selection"},
                          {"text", text},
                          {"generation", static_cast<qint64>(generation)}});
                });
        connect(&accessibleSelection, &AccessibilitySelection::availability, this,
                [](bool available, const QString &detail, quint64 generation) {
                    if (!available)
                        send({{"type", "selection_unavailable"},
                              {"text", detail},
                              {"generation", static_cast<qint64>(generation)}});
                });
        connect(&gnomeDesktop, &GnomeDesktop::availabilityChanged, this,
                [this](bool) { registerShortcuts(); });
        if (QGuiApplication::platformName() != "wayland") {
            connect(QApplication::clipboard(), &QClipboard::selectionChanged, this, [] {
                if (QApplication::activeWindow() == nullptr)
                    send({{"type", "selection"},
                          {"text", QApplication::clipboard()->text(QClipboard::Selection)}});
            });
            connect(QApplication::clipboard(), &QClipboard::dataChanged, this, [this] {
                if (config["watch_clipboard"].toBool() && QApplication::activeWindow() == nullptr &&
                    !clipboardChangeIsOurs())
                    send({{"type", "selection"}, {"text", QApplication::clipboard()->text()}});
            });
        }
        const QIcon icon(":/design/app-icon.svg");
        QApplication::setWindowIcon(icon);
        setWindowIcon(icon);
        QIcon trayIcon = icon;
#ifdef Q_OS_MACOS
        trayIcon = QIcon(":/design/tray-icon.svg");
        trayIcon.setIsMask(true);
#endif
        tray = new QSystemTrayIcon(trayIcon, this);
        tray->setToolTip(UiText::tr("Ranslator — ready"));
        auto *menu = new QMenu(this);
        menu->addAction(UiText::tr("Open translator"), this, [this] { showCard(); });
        menu->addSeparator();
        menu->addAction(UiText::tr("Select text once"), this, [] { action("once"); });
        menu->addAction(UiText::tr("Toggle continuous translation"), this,
                        [] { action("continuous"); });
        menu->addAction(UiText::tr("Snapshot"), this, [] { action("snapshot"); });
        menu->addAction(UiText::tr("Open image…"), this, [this] {
            QString path = QFileDialog::getOpenFileName(
                this, UiText::tr("Translate text in an image"), {},
                UiText::tr("Images") + " (*.png *.jpg *.jpeg *.webp *.bmp)");
            if (!path.isEmpty())
                loadImage(path);
        });
        menu->addSeparator();
        autostartAction = menu->addAction(UiText::tr("Start at login"));
        autostartAction->setObjectName("autostartAction");
        autostartAction->setCheckable(true);
        connect(autostartAction, &QAction::triggered, this,
                [this](bool enabled) { changeAutostart(enabled); });
        connect(menu, &QMenu::aboutToShow, autostart, &Autostart::refresh);
        connect(autostart, &Autostart::changed, this, [this] { syncAutostart(); });
        connect(autostart, &Autostart::failed, this, [this](const QString &error) {
            autostartError = error;
            syncAutostart();
            setStatus(error);
            if (!smokeMode)
                tray->showMessage(UiText::tr("Start at login"), error, QSystemTrayIcon::Warning,
                                  10000);
        });
        connect(qApp, &QGuiApplication::applicationStateChanged, this,
                [this](Qt::ApplicationState state) {
                    if (state == Qt::ApplicationActive)
                        autostart->refresh();
                });
        syncAutostart();
        menu->addAction(UiText::tr("Settings"), this, [this] { openSettings(); });
        menu->addAction(UiText::tr("Quit"), this, [] {
            send({{"type", "quit"}});
            qApp->quit();
        });
        tray->setContextMenu(menu);
        connect(tray, &QSystemTrayIcon::activated, this,
                [this](QSystemTrayIcon::ActivationReason reason) {
                    if (reason == QSystemTrayIcon::Trigger)
                        showCard();
                });
        if (!smokeMode)
            tray->show();
        quick = new QuickPopup(
            this, [this] { showCard(); },
            [this](QString source, QString target) {
                setLanguage(sourceLanguage, source);
                setLanguage(targetLanguage, target);
                quick->busy();
                translate();
            },
            [this] {
                quickSession = false;
                quickPending = false;
                ++quickPlacementGeneration;
                send({{"type", "dismiss_popup"}});
            });
        sourceText->setFocus();
        registerShortcuts();
    }
    void launch(const QJsonObject &command) {
        const auto image = command["_image"].toString();
        if (!image.isEmpty())
            loadImage(image);
        else if (command["_settings"].toBool())
            openSettings();
        else if (command["_quit"].toBool()) {
            send({{"type", "quit"}});
            qApp->quit();
        } else if (!command["_action"].toString().isEmpty()) {
            if (auto *recorder = ShortcutRecorder::active())
                recorder->interceptedShortcut();
            else
                action(command["_action"].toString());
        } else if (command["_window"].toBool() || !config["popup_mode"].toBool(true))
            showCard();
    }
    static void namedPreviewPage(QWidget *root, const QString &title) {
        for (auto *b : root->findChildren<QPushButton *>())
            if (b->text() == title) {
                b->click();
                return;
            }
    }
    void renderDesignPreview(const QString &directory) {
        auto save = [&directory](QWidget *widget, const QString &name) {
            auto image = Design::preview(widget);
            image.save(directory + "/" + name + ".png");
            image.save(directory + "/" + name + ".jpg", "JPG", 88);
        };
        config["preferred_language"] = "en";
        config["fallback_language"] = "zh";
        updateLanguageHint();
        setLanguage(sourceLanguage, "zh");
        sourceText->setPlainText("在时间存在之前，在任何事物存在之前，什么都没有。");
        translated->setPlainText(
            "Before there was time, before there was anything, there was nothing");
        copyButton->setEnabled(true);
        setLanguage(targetLanguage, "en");
        route->setText("CHINESE  →  ENGLISH");
        providerLabel->setText("●  On-device · Ollama");
        status->setText("Select anywhere with " + bindingDescription("once"));
        sourceText->clearFocus();
        config["appearance"] = "light";
        Design::configure(config);
        save(this, "translator-light");
        config["appearance"] = "dark";
        Design::configure(config);
        save(this, "translator-dark");
        quick->begin(config);
        quick->display(
            {{"text", "Before there was time, before there was anything, there was nothing"},
             {"source", "zh"},
             {"target", "en"}});
        save(quick, "quick-dark");
        quick->hide();
        config["appearance"] = "light";
        Design::configure(config);
        quick->begin(config);
        quick->display(
            {{"text", "Before there was time, before there was anything, there was nothing"},
             {"source", "zh"},
             {"target", "en"}});
        save(quick, "quick-light");
        quick->hide();
        openSettings();
        QTimer::singleShot(180, this, [this, directory] {
            if (!settings) {
                qApp->quit();
                return;
            }
            Design::preview(settings).save(directory + "/settings-light.png");
            Design::preview(settings).save(directory + "/settings-light.jpg", "JPG", 88);
            namedPreviewPage(settings, UiText::tr("Translation"));
            auto *provider = settings->findChild<QComboBox *>("providerSelect");
            provider->setCurrentIndex(provider->findData("ollama"));
            Design::preview(settings).save(directory + "/settings-translation.png");
            Design::preview(settings).save(directory + "/settings-translation.jpg", "JPG", 87);
            auto *pages = settings->findChild<QStackedWidget *>("settingsPages");
            pages->setCurrentIndex(0);
            config["appearance"] = "dark";
            Design::configure(config);
            Design::preview(settings).save(directory + "/settings-dark.png");
            Design::preview(settings).save(directory + "/settings-dark.jpg", "JPG", 88);
            qApp->quit();
        });
    }
    void receive(const QJsonObject &value) {
        const QString type = value["type"].toString();
        if (type == "snapshot_region") {
            if (overlay && overlay->isVisible())
                overlay->receiveRegion(value);
        } else if (type == "runtimes") {
            if (onRuntimes)
                onRuntimes(value);
        } else if (type == "models") {
            if (onModels)
                onModels(value);
        } else if (type.startsWith("packs_")) {
            if (onPacks)
                onPacks(value);
        } else if (type == "hide") {
            ++quickPlacementGeneration;
            quickPending = false;
            hide();
            if (quick)
                quick->hide();
            quickSession = false;
            syncSelectionFocus();
        } else if (type == "capture")
            capture();
        else if (type == "read_selection")
            readSelection();
        else if (type == "selection_missing") {
            const auto shortcut = bindingDescription("translate_selection");
            receive({{"type", "error"},
                     {"text",
                      shortcut.isEmpty()
                          ? UiText::tr("Select some text first, then try again.")
                          : UiText::tr("Select some text first, then press %1.").arg(shortcut)}});
        } else if (type == "busy") {
            working(true);
            setStatus(value["text"].toString());
        } else if (type == "selected") {
            translationRequest = value["request_id"].toString();
            const auto placement = ++quickPlacementGeneration;
            quickPending = false;
            sourceText->setPlainText(value["text"].toString());
            translated->clear();
            copyButton->setEnabled(false);
            setLanguage(sourceLanguage, "auto");
            setLanguage(targetLanguage, "auto");
            quickSession = config["popup_mode"].toBool(true);
            if (quickSession) {
                hide();
                if (gnomeDesktop.available() && !(overlay && overlay->isVisible())) {
                    quickPending = true;
                    quick->begin(config, nullptr, false);
                    gnomeDesktop.context([this, placement](QJsonObject context, const QString &) {
                        if (!quickSession || !quickPending || placement != quickPlacementGeneration)
                            return;
                        quickPending = false;
                        const QPoint cursor =
                            context.isEmpty()
                                ? QCursor::pos()
                                : QPoint(context["cursor_x"].toInt(), context["cursor_y"].toInt());
                        quick->showPrepared(GnomeDesktop::screenForContext(context), cursor);
                    });
                } else
                    quick->begin(config,
                                 overlay && overlay->isVisible() ? overlay->screen() : nullptr);
                quick->setAccessibleDescription(value["text"].toString());
            } else
                showCard();
            if (overlay)
                overlay->close();
        } else if (type == "translation") {
            if (value["request_id"].toString() != translationRequest)
                return;
            working(false);
            translatedSource = value["source"].toString();
            translatedTarget = value["target"].toString();
            translated->setFont(Design::textFont(translated->font(), translatedTarget));
            translated->setPlainText(value["text"].toString());
            Design::fadeIn(translated);
            if (quickSession && (quick->isVisible() || quickPending))
                quick->display(value);
            copyButton->setEnabled(true);
            route->setText(value["source"].toString().toUpper() + " → " +
                           value["target"].toString().toUpper());
            setStatus(UiText::tr("Ready · Change the target language to translate again."));
        } else if (type == "error") {
            if (value.contains("request_id") &&
                value["request_id"].toString() != translationRequest)
                return;
            working(false);
            setStatus(value["text"].toString());
            if (overlay && overlay->isVisible())
                overlay->showError(value["text"].toString());
            else if (quickSession && (quick->isVisible() || quickPending))
                quick->error(value["text"].toString());
            else if (config["popup_mode"].toBool(true)) {
                quickSession = true;
                quick->begin(config);
                quick->error(value["text"].toString());
            } else
                showCard();
        } else if (type == "cancelled") {
            ++quickPlacementGeneration;
            quickPending = false;
            ++captureGeneration;
            portal.cancelScreenshots();
            gnomeDesktop.cancelCapture();
            if (captureSelector)
                captureSelector->close();
            if (captureProcess)
                captureProcess->kill();
            if (overlay)
                overlay->close();
            working(false);
            setStatus(UiText::tr("Stopped. Your text stays here."));
        } else if (type == "provider")
            providerLabel->setText(value["text"].toString());
        else if (type == "listening") {
            QString mode = value["mode"].toString();
            nativeSelectionGeneration = value["generation"].toVariant().toULongLong();
            nativeSelectionArmed = mode != "off" && value["accessibility"].toBool();
            const auto generation = nativeSelectionGeneration;
            if (nativeSelectionArmed && gnomeDesktop.available() && !Capture::x11Available()) {
                accessibleSelection.setArmed(false, generation);
                gnomeDesktop.armSelection(
                    true, config["watch_clipboard"].toBool(),
                    [generation](const QString &text) {
                        send({{"type", "selection"},
                              {"text", text},
                              {"generation", static_cast<qint64>(generation)}});
                    },
                    [this, generation](const QString &error) {
                        if (nativeSelectionArmed && nativeSelectionGeneration == generation) {
                            setStatus(
                                UiText::tr("%1 Trying application accessibility.").arg(error));
                            accessibleSelection.setArmed(true, generation);
                        }
                    });
            } else {
                gnomeDesktop.armSelection(false, false, {});
                accessibleSelection.setArmed(nativeSelectionArmed, generation);
            }
            onceButton->setChecked(mode == "once");
            continuousButton->setChecked(mode == "continuous");
            continuousButton->setText(mode == "continuous" ? UiText::tr("Auto: on")
                                                           : UiText::tr("Auto translate"));
            continuousButton->setToolTip(
                UiText::tr(
                    "Translate each new text selection in other apps; click again to stop · %1")
                    .arg(bindingDescription("continuous")));
            stopButton->setEnabled(mode != "off" || progress->isVisible());
            QString text = mode == "once"         ? UiText::tr("Listening for your next selection")
                           : mode == "continuous" ? UiText::tr("Continuous translation is on")
                                                  : UiText::tr("Ready");
            tray->setToolTip(UiText::tr("Ranslator — %1").arg(text));
            setStatus(text);
        } else if (type == "ocr") {
            working(false);
            const auto path = value["path"].toString();
            if (overlay && path == screenshotPath)
                overlay->setWords(value["words"].toArray(), snapshotOffset);
            QFile::remove(path);
        } else if (type == "config_saved") {
            config = value["config"].toObject();
            Design::configure(config);
            if (settingsError) {
                settingsError->setStyleSheet("color: #4f8c73;");
                settingsError->setText(UiText::tr("Saved. You can keep changing settings."));
            }
            registerShortcuts();
            updateLanguageHint();
            setStatus(UiText::tr("Settings saved."));
            working(false);
        } else if (type == "config_error") {
            if (settingsError)
                settingsError->setText(value["text"].toString());
        } else if (type == "capability") {
            status->setToolTip(value["text"].toString());
        }
    }
};

static Window *window = nullptr;
extern "C" void ranslator_ui_emit(const char *raw) {
    if (!qApp)
        return;
    QByteArray data(raw);
    QMetaObject::invokeMethod(
        qApp,
        [data] {
            if (window)
                window->receive(QJsonDocument::fromJson(data).object());
        },
        Qt::QueuedConnection);
}
extern "C" int ranslator_ui_run(const char *settings, Callback cb, bool background,
                                const char *smokePath) {
    // QApplication remains alive until process exit, so queued Rust worker messages never target a
    // destroyed app.
    static int argc = 1;
    static char name[] = "ranslator";
    static char *argv[] = {name, nullptr};
#ifdef Q_OS_MACOS
    QCoreApplication::setAttribute(Qt::AA_MacDontSwapCtrlAndMeta);
#endif
    qputenv("QT_LINUX_ACCESSIBILITY_ALWAYS_ON", "1");
    auto *app = new QApplication(argc, argv);
    callback = cb;
    app->setApplicationName("ranslator");
    app->setOrganizationName("Ranslator");
    app->setDesktopFileName("io.github.ranslator.Ranslator");
    app->setQuitOnLastWindowClosed(false);
    I18n::install(*app);
    bool smoke = QString::fromUtf8(smokePath).size() > 0;
    QLockFile lock(QStandardPaths::writableLocation(QStandardPaths::TempLocation) + "/ranslator-" +
                   qEnvironmentVariable("USER") + ".lock");
    QLocalServer server;
    if (!smoke) {
        if (!lock.tryLock(0)) {
            QLocalSocket socket;
            socket.connectToServer("io.github.ranslator.Ranslator");
            if (socket.waitForConnected(1000)) {
                const auto startup = QJsonDocument::fromJson(settings).object();
                socket.write(QJsonDocument(QJsonObject{{"_image", startup["_image"]},
                                                       {"_action", startup["_action"]},
                                                       {"_quit", startup["_quit"]},
                                                       {"_settings", startup["_settings"]},
                                                       {"_window", startup["_window"]}})
                                 .toJson(QJsonDocument::Compact) +
                             "\n");
                socket.waitForBytesWritten(500);
            }
            return 0;
        }
        QLocalServer::removeServer("io.github.ranslator.Ranslator");
        server.setSocketOptions(QLocalServer::UserAccessOption);
        server.listen("io.github.ranslator.Ranslator");
    }
    Design::initialize();
    Design::configure(QJsonDocument::fromJson(settings).object());
    window = new Window(QJsonDocument::fromJson(settings).object(), smoke);
    QObject::connect(&server, &QLocalServer::newConnection, window, [&server] {
        while (auto *socket = server.nextPendingConnection()) {
            QObject::connect(socket, &QLocalSocket::readyRead, window, [socket] {
                if (socket->canReadLine()) {
                    const auto data = socket->readLine(65536);
                    window->launch(QJsonDocument::fromJson(data).object());
                    socket->disconnectFromServer();
                }
            });
            QObject::connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        }
    });
    if (smoke ||
        (!background && !QJsonDocument::fromJson(settings).object()["popup_mode"].toBool(true)) ||
        !QSystemTrayIcon::isSystemTrayAvailable())
        window->show();
    if (smoke) {
        QString output = QString::fromUtf8(smokePath);
        const auto preview =
            QJsonDocument::fromJson(settings).object()["_design_preview"].toString();
        QTimer::singleShot(300, window, [output, app, preview] {
            if (!preview.isEmpty())
                window->renderDesignPreview(preview);
            else {
                window->grab().save(output);
                const auto report =
                    QJsonDocument(
                        QJsonObject{
                            {"platform", QGuiApplication::platformName()},
                            {"desktop_blur_available", window->property("desktopBlur").toBool()},
                            {"translucent_window",
                             window->testAttribute(Qt::WA_TranslucentBackground)}})
                        .toJson(QJsonDocument::Compact);
                fprintf(stdout, "%s\n", report.constData());
                app->quit();
            }
        });
    } else {
        const auto startup = QJsonDocument::fromJson(settings).object();
        QTimer::singleShot(0, window, [startup] {
            send({{"type", "ready"}});
            if (startup.contains("_image") || startup.contains("_quit") ||
                startup.contains("_settings") || startup.contains("_window") ||
                startup.contains("_action"))
                window->launch(startup);
        });
    }
    int result = app->exec();
    auto *finished = window;
    window = nullptr;
    delete finished;
    return result;
}
