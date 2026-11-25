import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:webview_windows/webview_windows.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  const plugin = MethodChannel('io.jns.webview.win');
  const events = 'io.jns.webview.win/1/events';
  const codec = StandardMethodCodec();
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  late WebviewController controller;
  late List<WebviewProcessFailure> failures;

  Future<void> processFailed(Map<String, Object?> value) async {
    await messenger.handlePlatformMessage(
        events,
        codec.encodeSuccessEnvelope({'type': 'processFailed', 'value': value}),
        (_) {});
    await pumpEventQueue();
  }

  setUp(() async {
    messenger.setMockMethodCallHandler(
        plugin,
        (call) async => call.method == 'initialize'
            ? {'textureId': 1, 'isHttpAuthSupported': true}
            : null);
    messenger.setMockMethodCallHandler(
        const MethodChannel(events), (_) async => null);
    controller = WebviewController();
    await controller.initialize();
    failures = [];
    controller.onProcessFailed.listen(failures.add);
  });

  tearDown(() {
    messenger.setMockMethodCallHandler(const MethodChannel(events), null);
    messenger.setMockMethodCallHandler(plugin, null);
  });

  test('reports the details of a failed process', () async {
    await processFailed({
      'kind': 0,
      'reason': 3,
      'exitCode': -1073741819,
      'processDescription': 'Browser',
    });
    expect(failures.single.kind, WebviewProcessFailedKind.browserProcessExited);
    expect(failures.single.reason, WebviewProcessFailedReason.crashed);
    expect(failures.single.exitCode, -1073741819);
    expect(failures.single.processDescription, 'Browser');
    await controller.dispose();
  });

  test('reports a failure without details', () async {
    await processFailed({
      'kind': 6,
      'reason': null,
      'exitCode': null,
      'processDescription': '',
    });
    expect(failures.single.kind, WebviewProcessFailedKind.gpuProcessExited);
    expect(failures.single.reason, isNull);
    expect(failures.single.exitCode, isNull);
    await controller.dispose();
  });

  test('reports values that a newer runtime added', () async {
    await processFailed({
      'kind': 1000,
      'reason': 1000,
      'exitCode': 1,
      'processDescription': '',
    });
    expect(failures.single.kind, WebviewProcessFailedKind.unknownProcessExited);
    expect(failures.single.reason, isNull);
    await controller.dispose();
  });

  test('closes the stream on dispose', () async {
    final done = expectLater(controller.onProcessFailed, emitsDone);
    await controller.dispose();
    await done;
  });
}
