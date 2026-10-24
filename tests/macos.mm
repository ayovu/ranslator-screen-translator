// macOS adapter regressions that need Objective-C. They use a private, uniquely named pasteboard,
// never the user's clipboard.
#include "../native/macos.hpp"
#import <AppKit/AppKit.h>
#include <QtTest>

class MacTests : public QObject {
    Q_OBJECT
  private slots:
    void translate_selection_restores_every_pasteboard_item_and_representation() {
        NSPasteboard *board = [NSPasteboard pasteboardWithUniqueName];
        NSData *html = [@"<b>Synthetic</b>" dataUsingEncoding:NSUTF8StringEncoding];
        NSData *custom = [NSData dataWithBytes:"\x01\x02\x03" length:3];
        NSPasteboardItem *first = [NSPasteboardItem new];
        [first setString:@"Synthetic clipboard" forType:NSPasteboardTypeString];
        [first setData:html forType:NSPasteboardTypeHTML];
        NSPasteboardItem *second = [NSPasteboardItem new];
        [second setData:custom forType:@"io.github.ranslator.test"];
        NSArray<NSPasteboardItem *> *items = @[ first, second ];
        [board clearContents];
        QVERIFY([board writeObjects:items]);
        auto restore = NativeMac::preservePasteboard(board);
        // The ⌘C fallback replaces the contents with the selected text.
        [board clearContents];
        [board setString:@"Copied selection" forType:NSPasteboardTypeString];
        restore();
        QCOMPARE(board.pasteboardItems.count, NSUInteger(2));
        QCOMPARE(
            QString::fromNSString([board.pasteboardItems[0] stringForType:NSPasteboardTypeString]),
            QString("Synthetic clipboard"));
        QVERIFY([[board.pasteboardItems[0] dataForType:NSPasteboardTypeHTML] isEqual:html]);
        QVERIFY(
            [[board.pasteboardItems[1] dataForType:@"io.github.ranslator.test"] isEqual:custom]);
        // An empty clipboard stays empty instead of keeping the copied selection.
        [board clearContents];
        auto restoreEmpty = NativeMac::preservePasteboard(board);
        [board setString:@"Copied selection" forType:NSPasteboardTypeString];
        restoreEmpty();
        QCOMPARE(board.pasteboardItems.count, NSUInteger(0));
        [board releaseGlobally];
    }
};

QTEST_MAIN(MacTests)
#include "macos.moc"
