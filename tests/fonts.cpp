#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QFontDatabase>
#include <QGlyphRun>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QRawFont>
#include <QTextLayout>

// This probe is also run in minimal distribution containers with all OS fonts hidden.
// Glyph indexes verify shaping, while the PNG lets reviewers inspect actual script rendering.
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QFontDatabase::addApplicationFont(":/design/InterVariable.ttf");
    if (!app.arguments().contains("--without-fallback")) {
        const QDir fonts(":/design/fonts");
        for (const auto &name : fonts.entryList({"*.ttf", "*.ttc"}, QDir::Files)) {
            if (QFontDatabase::addApplicationFont(fonts.filePath(name)) < 0)
                qFatal("Could not register bundled fallback font");
        }
    }
    QFont font("Inter Variable");
    font.setPixelSize(32);
    font.setWeight(QFont::Normal);
    const QStringList samples{
        "English — Français — Tiếng Việt — Ελληνικά — Русский",
        "简体中文：选择屏幕上的文字并翻译。",
        "繁體中文：選擇螢幕上的文字並翻譯。",
        "日本語：画面のテキストを選択して翻訳します。",
        "한국어: 화면의 텍스트를 선택하고 번역합니다.",
        "العربية: حدد النص على الشاشة ثم ترجمه.",
        "فارسی: متن روی صفحه را انتخاب و ترجمه کنید.",
        "हिन्दी: स्क्रीन पर पाठ चुनें और अनुवाद करें।",
        "עברית: בחר טקסט על המסך ותרגם אותו.",
        "বাংলা: পর্দায় লেখা নির্বাচন করুন এবং অনুবাদ করুন।",
        "ภาษาไทย: เลือกข้อความบนหน้าจอแล้วแปล",
    };
    QImage canvas(1440, 90 + samples.size() * 76, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(QColor("#f7f9ff"));
    QPainter painter(&canvas);
    painter.setPen(QColor("#132133"));
    painter.setRenderHint(QPainter::TextAntialiasing);
    QJsonArray rows;
    bool complete = true;
    qreal y = 30;
    for (const auto &sample : samples) {
        QTextLayout layout(sample, font);
        layout.beginLayout();
        auto line = layout.createLine();
        line.setLineWidth(1360);
        layout.endLayout();
        QJsonArray families;
        int missing = 0, glyphs = 0;
        for (const auto &run : layout.glyphRuns()) {
            families.append(run.rawFont().familyName());
            for (const auto glyph : run.glyphIndexes()) {
                ++glyphs;
                if (!glyph)
                    ++missing;
            }
        }
        complete &= missing == 0 && glyphs > 0;
        layout.draw(&painter, QPointF(36, y));
        y += 76;
        rows.append(QJsonObject{{"text", sample},
                                {"font_families", families},
                                {"glyphs", glyphs},
                                {"missing_glyphs", missing}});
    }
    painter.end();
    if (app.arguments().size() > 1 && !app.arguments()[1].startsWith("--") &&
        !canvas.save(app.arguments()[1]))
        return 2;
    qInfo().noquote() << QJsonDocument(QJsonObject{{"complete", complete}, {"samples", rows}})
                             .toJson(QJsonDocument::Compact);
    return complete ? 0 : 1;
}
