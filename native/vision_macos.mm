#include "macos.hpp"
#import <CoreText/CoreText.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <Vision/Vision.h>
#include <algorithm>
#include <cstdio>
#include <pthread/qos.h>

namespace {
// OCR settings keep Tesseract codes; Vision expects BCP 47 identifiers.
NSArray<NSString *> *visionLanguages(const QString &languages) {
    static const QHash<QString, QString> codes{
        {"eng", "en-US"}, {"fra", "fr-FR"}, {"ita", "it-IT"},       {"deu", "de-DE"},
        {"spa", "es-ES"}, {"por", "pt-BR"}, {"chi_sim", "zh-Hans"}, {"chi_tra", "zh-Hant"},
        {"kor", "ko-KR"}, {"jpn", "ja-JP"}, {"rus", "ru-RU"},       {"ukr", "uk-UA"},
        {"tha", "th-TH"}, {"vie", "vi-VT"}, {"ara", "ar-SA"},       {"tur", "tr-TR"},
        {"ind", "id-ID"}, {"ces", "cs-CZ"}, {"dan", "da-DK"},       {"nld", "nl-NL"},
        {"nor", "no-NO"}, {"msa", "ms-MY"}, {"pol", "pl-PL"},       {"ron", "ro-RO"},
        {"swe", "sv-SE"}};
    NSMutableArray<NSString *> *result = [NSMutableArray array];
    for (const auto &code : languages.split('+', Qt::SkipEmptyParts))
        if (codes.contains(code))
            [result addObject:codes[code].toNSString()];
    return result;
}

struct Line {
    QRectF box;
    VNRecognizedText *text;
};

QRectF pixelRect(CGRect normalized, QSizeF size) {
    // Vision reports normalized coordinates with a bottom-left origin.
    return QRectF(normalized.origin.x * size.width(),
                  (1 - normalized.origin.y - normalized.size.height) * size.height(),
                  normalized.size.width * size.width(), normalized.size.height * size.height());
}

// Group lines whose vertical centers fall inside one another into rows, then read left to right.
QList<Line> readingOrder(QList<Line> lines) {
    std::sort(lines.begin(), lines.end(),
              [](const Line &a, const Line &b) { return a.box.top() < b.box.top(); });
    QList<QList<Line>> rows;
    for (const auto &line : lines) {
        if (!rows.isEmpty()) {
            const auto &first = rows.last().first().box;
            if (line.box.center().y() >= first.top() && line.box.center().y() <= first.bottom()) {
                rows.last().append(line);
                continue;
            }
        }
        rows.append({line});
    }
    QList<Line> ordered;
    for (auto &row : rows) {
        std::sort(row.begin(), row.end(),
                  [](const Line &a, const Line &b) { return a.box.left() < b.box.left(); });
        ordered.append(row);
    }
    return ordered;
}

VNRecognizeTextRequest *textRequest(const QString &languages) {
    VNRecognizeTextRequest *request = [VNRecognizeTextRequest new];
    request.recognitionLevel = VNRequestTextRecognitionLevelAccurate;
    request.usesLanguageCorrection = YES;
    // Screens mix languages; configured OCR languages are hints, not a whitelist.
    request.automaticallyDetectsLanguage = YES;
    // The default skips text below 1/32 of the image height: most text on a full display.
    request.minimumTextHeight = 0;
    if (auto hints = visionLanguages(languages); hints.count)
        request.recognitionLanguages = hints;
    return request;
}

QString field(QString text) {
    return text.replace('\t', ' ').replace('\n', ' ').replace('\r', ' ');
}
} // namespace

namespace NativeMac {
QString recognizeText(const QString &imagePath, const QString &languages, QString *error) {
    @autoreleasepool {
        NSURL *url = [NSURL fileURLWithPath:imagePath.toNSString()];
        CGImageSourceRef source = CGImageSourceCreateWithURL((__bridge CFURLRef)url, nullptr);
        CGImageRef image = source ? CGImageSourceCreateImageAtIndex(source, 0, nullptr) : nullptr;
        if (source)
            CFRelease(source);
        if (!image) {
            *error = "Could not read the captured image.";
            return {};
        }
        const QSizeF size(CGImageGetWidth(image), CGImageGetHeight(image));
        VNRecognizeTextRequest *request = textRequest(languages);
        VNImageRequestHandler *handler = [[VNImageRequestHandler alloc] initWithCGImage:image
                                                                                options:@{}];
        NSError *failure = nil;
        const bool performed = [handler performRequests:@[ request ] error:&failure];
        CGImageRelease(image);
        if (!performed) {
            *error = "Apple Vision could not recognize text. " +
                     QString::fromNSString(failure.localizedDescription);
            return {};
        }
        QList<Line> lines;
        for (VNRecognizedTextObservation *observation in request.results) {
            VNRecognizedText *text = [observation topCandidates:1].firstObject;
            if (text && text.string.length)
                lines.append({pixelRect(observation.boundingBox, size), text});
        }
        // Tesseract's TSV layout: the shared parser reads level-5 rows and groups by line key.
        QString tsv = "level\tpage_num\tblock_num\tpar_num\tline_num\tword_num\tleft\ttop\twidth\t"
                      "height\tconf\ttext\n";
        int lineNumber = 0;
        NSCharacterSet *spaces = NSCharacterSet.whitespaceAndNewlineCharacterSet;
        for (const auto &line : readingOrder(lines)) {
            ++lineNumber;
            NSString *string = line.text.string;
            const auto confidence = qRound(line.text.confidence * 100);
            int wordNumber = 0;
            NSUInteger index = 0;
            while (index < string.length) {
                const auto start =
                    [string rangeOfCharacterFromSet:spaces.invertedSet
                                            options:0
                                              range:NSMakeRange(index, string.length - index)];
                if (start.location == NSNotFound)
                    break;
                auto end =
                    [string rangeOfCharacterFromSet:spaces
                                            options:0
                                              range:NSMakeRange(start.location,
                                                                string.length - start.location)];
                const NSUInteger stop = end.location == NSNotFound ? string.length : end.location;
                const auto range = NSMakeRange(start.location, stop - start.location);
                index = stop;
                QRectF box = line.box;
                if (VNRectangleObservation *word = [line.text boundingBoxForRange:range error:nil])
                    box = pixelRect(word.boundingBox, size);
                const QRect pixels = box.toAlignedRect().intersected(
                    QRect(0, 0, int(size.width()), int(size.height())));
                if (pixels.isEmpty())
                    continue;
                tsv += QString("5\t1\t1\t1\t%1\t%2\t%3\t%4\t%5\t%6\t%7\t%8\n")
                           .arg(lineNumber)
                           .arg(++wordNumber)
                           .arg(pixels.x())
                           .arg(pixels.y())
                           .arg(pixels.width())
                           .arg(pixels.height())
                           .arg(confidence)
                           .arg(field(QString::fromNSString([string substringWithRange:range])));
            }
        }
        return tsv;
    }
}

bool warmUpTextRecognition(const QString &languages) {
    @autoreleasepool {
        // Recognizing real glyphs loads the same models a Snapshot needs; a blank image does not.
        constexpr size_t width = 480, height = 96;
        CGColorSpaceRef color = CGColorSpaceCreateDeviceRGB();
        CGContextRef context = CGBitmapContextCreate(nullptr, width, height, 8, 0, color,
                                                     kCGImageAlphaPremultipliedLast);
        CGColorSpaceRelease(color);
        if (!context)
            return false;
        CGContextSetRGBFillColor(context, 1, 1, 1, 1);
        CGContextFillRect(context, CGRectMake(0, 0, width, height));
        CTFontRef font = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, 28, nullptr);
        NSDictionary *attributes = @{
            (__bridge NSString *)kCTFontAttributeName : (__bridge id)font,
            (__bridge NSString *)kCTForegroundColorFromContextAttributeName : @YES
        };
        NSAttributedString *sample =
            [[NSAttributedString alloc] initWithString:@"Ranslator 2026 Text"
                                            attributes:attributes];
        CTLineRef line = CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)sample);
        CGContextSetRGBFillColor(context, 0, 0, 0, 1);
        CGContextSetTextPosition(context, 20, 36);
        CTLineDraw(line, context);
        CFRelease(line);
        CFRelease(font);
        CGImageRef image = CGBitmapContextCreateImage(context);
        CGContextRelease(context);
        if (!image)
            return false;
        VNImageRequestHandler *handler = [[VNImageRequestHandler alloc] initWithCGImage:image
                                                                                options:@{}];
        const bool performed = [handler performRequests:@[ textRequest(languages) ] error:nil];
        CGImageRelease(image);
        return performed;
    }
}
} // namespace NativeMac

// `ranslator --vision-ocr IMAGE LANGUAGES` runs this in a child process. The shared OCR
// scheduler keeps its timeout and cancellation by terminating that process.
extern "C" int ranslator_vision_ocr(const char *image, const char *languages) {
    QString error;
    const auto tsv =
        NativeMac::recognizeText(QString::fromUtf8(image), QString::fromUtf8(languages), &error);
    if (!error.isEmpty()) {
        std::fprintf(stderr, "%s\n", error.toUtf8().constData());
        return 1;
    }
    const auto bytes = tsv.toUtf8();
    std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout);
    std::fflush(stdout);
    return 0;
}

// `ranslator --vision-ocr --warm-up LANGUAGES` compiles Vision's models below Snapshot priority.
extern "C" int ranslator_vision_warm_up(const char *languages) {
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
    return NativeMac::warmUpTextRecognition(QString::fromUtf8(languages)) ? 0 : 1;
}
