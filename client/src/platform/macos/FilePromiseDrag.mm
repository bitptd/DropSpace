#include "platform/macos/FilePromiseDrag.h"

#include "core/SafeExtractor.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QWidget>
#include <QUuid>

#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <atomic>
#include <mutex>
#include <set>

namespace dropspace::macos {
namespace {

NSString* nativeText(const QString& value) {
  return [NSString stringWithCharacters:reinterpret_cast<const unichar*>(value.utf16())
                                  length:static_cast<NSUInteger>(value.size())];
}

QString qtText(NSString* value) {
  if (!value) return {};
  return QString::fromNSString(value);
}

QString safeExportName(QString name, bool folder) {
  name = QFileInfo(name).fileName().normalized(QString::NormalizationForm_C);
  static const QString invalid = QStringLiteral("/:\\\0");
  for (const QChar ch : invalid) name.replace(ch, u'_');
  for (qsizetype index = 0; index < name.size(); ++index) {
    if (name[index].unicode() < 32) name[index] = u'_';
  }
  name = name.trimmed();
  if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral(".."))
    name = folder ? QStringLiteral("DropSpace Folder") : QStringLiteral("DropSpace File");
  return name.left(240);
}

struct PromiseSession {
  explicit PromiseSession(std::vector<FilePromiseEntry> frozen)
      : entries(std::move(frozen)), remaining(static_cast<int>(entries.size())) {}

  std::vector<FilePromiseEntry> entries;
  std::atomic<int> remaining;
  std::atomic<int> started{0};
  std::atomic<bool> cancelled{false};
};

std::mutex sessionsMutex;
std::set<PromiseSession*> activeSessions;
NSEvent* lastLeftMouseEvent = nil;
bool leftMouseButtonDown = false;
id mouseEventMonitor = nil;

void registerSession(PromiseSession* session) {
  std::lock_guard lock(sessionsMutex);
  activeSessions.insert(session);
}

void unregisterSession(PromiseSession* session) {
  std::lock_guard lock(sessionsMutex);
  activeSessions.erase(session);
}

NSError* promiseError(const QString& message) {
  return [NSError errorWithDomain:@"com.dropspace.client.file-promise"
                             code:1
                         userInfo:@{NSLocalizedDescriptionKey : nativeText(message)}];
}

NSString* typeForEntry(const FilePromiseEntry& entry) {
  if (entry.folder) return UTTypeFolder.identifier;
  if (!entry.fileType.isEmpty()) {
    UTType* explicitType = [UTType typeWithIdentifier:nativeText(entry.fileType)];
    if (explicitType) return explicitType.identifier;
  }
  const QString extension = QFileInfo(entry.fileName).suffix();
  if (!extension.isEmpty()) {
    UTType* inferred = [UTType typeWithFilenameExtension:nativeText(extension)];
    if (inferred) return inferred.identifier;
  }
  return UTTypeData.identifier;
}

bool writeRegularFile(const QString& source, const QString& destination, QString* error) {
  if (QFileInfo::exists(destination)) {
    *error = QStringLiteral("Finder destination already exists");
    return false;
  }
  const QString part = QDir(QFileInfo(destination).absolutePath()).filePath(
      QStringLiteral(".%1.dropspace-%2.part")
          .arg(QFileInfo(destination).fileName(),
               QUuid::createUuid().toString(QUuid::WithoutBraces)));
  QFile::remove(part);
  if (!QFile::copy(source, part)) {
    *error = QStringLiteral("Could not copy verified object to Finder temporary file");
    return false;
  }
  if (!QFile::rename(part, destination)) {
    QFile::remove(part);
    *error = QStringLiteral("Could not atomically publish Finder file");
    return false;
  }
  return true;
}

}  // namespace
}  // namespace dropspace::macos

@class DSFilePromiseOwner;

@interface DSFilePromiseDelegate : NSObject <NSFilePromiseProviderDelegate>
- (instancetype)initWithSession:(std::shared_ptr<dropspace::macos::PromiseSession>)session
                           index:(NSUInteger)index;
@end

@interface DSFilePromiseSource : NSObject <NSDraggingSource>
- (instancetype)initWithSession:(std::shared_ptr<dropspace::macos::PromiseSession>)session;
@end

@interface DSFilePromiseOwner : NSObject
@property(nonatomic) std::shared_ptr<dropspace::macos::PromiseSession> session;
@property(nonatomic, strong) NSMutableArray* keepAlive;
@end

static NSMutableArray<DSFilePromiseOwner*>* promiseOwners() {
  static NSMutableArray<DSFilePromiseOwner*>* owners;
  static dispatch_once_t once;
  dispatch_once(&once, ^{ owners = [NSMutableArray array]; });
  return owners;
}

static void releasePromiseOwner(dropspace::macos::PromiseSession* session) {
  dispatch_async(dispatch_get_main_queue(), ^{
    NSMutableArray* owners = promiseOwners();
    for (NSInteger index = owners.count - 1; index >= 0; --index) {
      DSFilePromiseOwner* owner = owners[static_cast<NSUInteger>(index)];
      if (owner.session.get() == session) {
        dropspace::macos::unregisterSession(session);
        [owners removeObjectAtIndex:static_cast<NSUInteger>(index)];
      }
    }
  });
}

@implementation DSFilePromiseOwner
@end

@implementation DSFilePromiseDelegate {
  std::shared_ptr<dropspace::macos::PromiseSession> _session;
  NSUInteger _index;
}

- (instancetype)initWithSession:(std::shared_ptr<dropspace::macos::PromiseSession>)session
                           index:(NSUInteger)index {
  self = [super init];
  if (self) {
    _session = std::move(session);
    _index = index;
  }
  return self;
}

- (NSString*)filePromiseProvider:(NSFilePromiseProvider*)provider
              fileNameForType:(NSString*)fileType {
  Q_UNUSED(provider);
  Q_UNUSED(fileType);
  return dropspace::macos::nativeText(_session->entries[_index].fileName);
}

- (NSOperationQueue*)operationQueueForFilePromiseProvider:(NSFilePromiseProvider*)provider {
  Q_UNUSED(provider);
  static NSOperationQueue* queue;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    queue = [[NSOperationQueue alloc] init];
    queue.name = @"com.dropspace.client.file-promises";
    queue.maxConcurrentOperationCount = 2;
    queue.qualityOfService = NSQualityOfServiceUserInitiated;
  });
  return queue;
}

- (void)filePromiseProvider:(NSFilePromiseProvider*)provider
          writePromiseToURL:(NSURL*)destinationURL
          completionHandler:(void (^)(NSError* _Nullable))completionHandler {
  Q_UNUSED(provider);
  _session->started.fetch_add(1);
  const auto& entry = _session->entries[_index];
  QString failure;
  if (_session->cancelled.load()) {
    failure = QStringLiteral("File promise was cancelled");
  } else {
    const QString object = entry.acquireObject ? entry.acquireObject(&failure) : QString();
    if (failure.isEmpty() && object.isEmpty())
      failure = QStringLiteral("Content provider returned no verified object");
    if (failure.isEmpty()) {
      // AppKit supplies the final promised-item URL here, not its parent
      // directory.  Coordinate that exact URL and publish via a sibling
      // temporary item so Finder never observes partial content.
      NSFileCoordinator* coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
      __block bool accessorRan = false;
      __block QString coordinatedFailure;
      NSError* coordinationError = nil;
      [coordinator coordinateWritingItemAtURL:destinationURL
                                      options:0
                                        error:&coordinationError
                                   byAccessor:^(NSURL* coordinatedURL) {
        accessorRan = true;
        const QString destination = dropspace::macos::qtText(coordinatedURL.path);
        if (entry.folder) {
          std::vector<dropspace::ExtractedEntry> manifest;
          if (!dropspace::safeExtractZip(object, destination, &manifest,
                                         &coordinatedFailure)) {
            coordinatedFailure = QStringLiteral("Safe folder delivery failed: %1")
                                     .arg(coordinatedFailure);
          }
        } else {
          dropspace::macos::writeRegularFile(object, destination, &coordinatedFailure);
        }
      }];
      if (!coordinatedFailure.isEmpty()) {
        failure = coordinatedFailure;
      } else if (coordinationError) {
        failure = QStringLiteral("Finder file coordination failed: %1")
                      .arg(dropspace::macos::qtText(coordinationError.localizedDescription));
      } else if (!accessorRan) {
        failure = QStringLiteral("Finder file coordinator did not provide a destination");
      }
    }
  }

  completionHandler(failure.isEmpty() ? nil : dropspace::macos::promiseError(failure));
  if (_session->remaining.fetch_sub(1) == 1) releasePromiseOwner(_session.get());
}

@end

@implementation DSFilePromiseSource {
  std::shared_ptr<dropspace::macos::PromiseSession> _session;
}

- (instancetype)initWithSession:(std::shared_ptr<dropspace::macos::PromiseSession>)session {
  self = [super init];
  if (self) _session = std::move(session);
  return self;
}

- (NSDragOperation)draggingSession:(NSDraggingSession*)session
    sourceOperationMaskForDraggingContext:(NSDraggingContext)context {
  Q_UNUSED(session);
  Q_UNUSED(context);
  return NSDragOperationCopy;
}

- (BOOL)ignoreModifierKeysForDraggingSession:(NSDraggingSession*)session {
  Q_UNUSED(session);
  return YES;
}

- (void)draggingSession:(NSDraggingSession*)session
            endedAtPoint:(NSPoint)screenPoint
               operation:(NSDragOperation)operation {
  Q_UNUSED(session);
  Q_UNUSED(screenPoint);
  if (operation == NSDragOperationNone && _session->started.load() == 0) {
    _session->cancelled.store(true);
    releasePromiseOwner(_session.get());
  }
}

@end

namespace dropspace::macos {

void installFilePromiseEventMonitor() {
  if (mouseEventMonitor) return;
  mouseEventMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:
      (NSEventMaskLeftMouseDown | NSEventMaskLeftMouseDragged | NSEventMaskLeftMouseUp)
      handler:^NSEvent* _Nullable(NSEvent* event) {
    if (event.type == NSEventTypeLeftMouseDown || event.type == NSEventTypeLeftMouseDragged) {
      lastLeftMouseEvent = event;
      leftMouseButtonDown = true;
    } else if (event.type == NSEventTypeLeftMouseUp) {
      leftMouseButtonDown = false;
    }
    return event;
  }];
}

bool startFilePromiseDrag(QWidget* source, std::vector<FilePromiseEntry> entries,
                          QString* error) {
  if (error) error->clear();
  if (!source || entries.empty()) {
    if (error) *error = QStringLiteral("No source widget or promised entries");
    return false;
  }
  if (![NSThread isMainThread]) {
    if (error) *error = QStringLiteral("File Promise drag must start on the AppKit main thread");
    return false;
  }
  NSEvent* event = NSApp.currentEvent;
  if (!event || (event.type != NSEventTypeLeftMouseDragged &&
                 event.type != NSEventTypeLeftMouseDown)) {
    event = leftMouseButtonDown ? lastLeftMouseEvent : nil;
  }
  if (!event) {
    if (error) *error = QStringLiteral("File Promise drag requires the current mouse drag event");
    return false;
  }

  std::set<QString> used;
  for (auto& entry : entries) {
    QString candidate = safeExportName(entry.fileName, entry.folder);
    const QFileInfo info(candidate);
    const QString stem = entry.folder || info.suffix().isEmpty() ? candidate : info.completeBaseName();
    const QString extension = entry.folder || info.suffix().isEmpty() ? QString() : u'.' + info.suffix();
    int suffix = 2;
    while (!used.insert(candidate.toCaseFolded()).second)
      candidate = QStringLiteral("%1 (%2)%3").arg(stem).arg(suffix++).arg(extension);
    entry.fileName = candidate;
  }

  auto session = std::make_shared<PromiseSession>(std::move(entries));
  registerSession(session.get());
  DSFilePromiseOwner* owner = [[DSFilePromiseOwner alloc] init];
  owner.session = session;
  owner.keepAlive = [NSMutableArray array];
  DSFilePromiseSource* dragSource = [[DSFilePromiseSource alloc] initWithSession:session];
  [owner.keepAlive addObject:dragSource];

  source->setAttribute(Qt::WA_NativeWindow, true);
  NSView* view = (__bridge NSView*)reinterpret_cast<void*>(source->winId());
  if (!view) {
    unregisterSession(session.get());
    if (error) *error = QStringLiteral("Qt did not expose a native NSView for the drag source");
    return false;
  }
  if (event.window != view.window) {
    unregisterSession(session.get());
    if (error) *error = QStringLiteral("File Promise mouse event belongs to another window");
    return false;
  }

  NSMutableArray<NSDraggingItem*>* items = [NSMutableArray array];
  NSPoint location = [view convertPoint:event.locationInWindow fromView:nil];
  for (NSUInteger index = 0; index < session->entries.size(); ++index) {
    const auto& entry = session->entries[index];
    DSFilePromiseDelegate* delegate = [[DSFilePromiseDelegate alloc] initWithSession:session
                                                                               index:index];
    NSFilePromiseProvider* provider = [[NSFilePromiseProvider alloc]
        initWithFileType:typeForEntry(entry) delegate:delegate];
    [owner.keepAlive addObject:delegate];
    [owner.keepAlive addObject:provider];
    NSDraggingItem* item = [[NSDraggingItem alloc] initWithPasteboardWriter:provider];
    NSImage* image = entry.folder
        ? [NSWorkspace.sharedWorkspace iconForContentType:UTTypeFolder]
        : [NSWorkspace.sharedWorkspace iconForContentType:[UTType typeWithIdentifier:typeForEntry(entry)]];
    const CGFloat offset = static_cast<CGFloat>(index) * 3.0;
    [item setDraggingFrame:NSMakeRect(location.x - 28.0 + offset,
                                     location.y - 28.0 - offset, 56.0, 56.0)
                 contents:image];
    [items addObject:item];
  }

  [promiseOwners() addObject:owner];
  NSDraggingSession* nativeSession = [view beginDraggingSessionWithItems:items
                                                                    event:event
                                                                   source:dragSource];
  nativeSession.animatesToStartingPositionsOnCancelOrFail = YES;
  return true;
}

int activeFilePromiseCount() {
  std::lock_guard lock(sessionsMutex);
  return static_cast<int>(activeSessions.size());
}

}  // namespace dropspace::macos
