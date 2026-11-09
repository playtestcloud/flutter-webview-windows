import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:webview_windows/webview_windows.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  const channel = MethodChannel('io.jns.webview.win/1');
  const events = 'io.jns.webview.win/1/events';
  const codec = StandardMethodCodec();
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  late WebviewController controller;
  late List<MethodCall> calls;
  late List<WebviewHttpAuthRequest> requests;

  Future<void> challenge(int id,
      {String value = 'Basic realm="Prototype"',
      String uri = 'https://example.com/game'}) async {
    await messenger.handlePlatformMessage(
        events,
        codec.encodeSuccessEnvelope({
          'type': 'httpAuthRequested',
          'value': {
            'id': id,
            'uri': uri,
            'challenge': value
          },
        }),
        (_) {});
    await pumpEventQueue();
  }

  setUp(() async {
    calls = [];
    requests = [];
    messenger.setMockMethodCallHandler(
        const MethodChannel('io.jns.webview.win'),
        (call) async => call.method == 'initialize'
            ? {'textureId': 1, 'isHttpAuthSupported': true}
            : null);
    messenger.setMockMethodCallHandler(
        const MethodChannel(events), (_) async => null);
    messenger.setMockMethodCallHandler(channel, (call) async {
      calls.add(call);
      return null;
    });
    controller = WebviewController();
    await controller.initialize();
    controller.onHttpAuthRequest = requests.add;
  });

  tearDown(() async {
    await controller.dispose();
    messenger.setMockMethodCallHandler(channel, null);
    messenger.setMockMethodCallHandler(const MethodChannel(events), null);
    messenger.setMockMethodCallHandler(
        const MethodChannel('io.jns.webview.win'), null);
  });

  test('keeps independent challenges pending until each is answered', () async {
    await challenge(1);
    await challenge(2);
    expect(requests, hasLength(2));
    expect(requests.first.host, 'example.com');
    expect(requests.first.realm, 'Prototype');
    expect(calls, isEmpty);
    expect(controller.isHttpAuthSupported, isTrue);
    await requests.last.onProceed(
        const WebviewHttpAuthCredential(user: 'foo', password: 'bar'));
    await requests.first.onCancel();
    expect(calls.map((call) => call.arguments), [
      {'id': 2, 'user': 'foo', 'password': 'bar'},
      {'id': 1},
    ]);
  });

  test('answers a challenge at most once, including empty credentials',
      () async {
    await challenge(1);
    await Future.wait([
      requests.single
          .onProceed(const WebviewHttpAuthCredential(user: '', password: '')),
      requests.single.onCancel(),
      requests.single.onProceed(
          const WebviewHttpAuthCredential(user: 'late', password: 'late')),
    ]);
    expect(
        calls.where((call) => call.method == 'completeHttpAuth'), hasLength(1));
    expect(calls.last.arguments, {'id': 1, 'user': '', 'password': ''});
  });

  test('cancels when the handler is absent or throws', () async {
    controller.onHttpAuthRequest = null;
    await challenge(1);
    controller.onHttpAuthRequest = (_) => throw StateError('handler failed');
    await challenge(2);
    expect(
        calls
            .where((call) => call.method == 'completeHttpAuth')
            .map((call) => call.arguments),
        [
          {'id': 1},
          {'id': 2},
        ]);
  });

  test('cancels when the request URI cannot be parsed', () async {
    await challenge(1, uri: 'http://[::1');
    expect(requests, isEmpty);
    expect(calls.last.arguments, {'id': 1});
  });

  test('cancels when an asynchronous handler fails', () async {
    controller.onHttpAuthRequest = (_) async {
      await Future<void>.delayed(Duration.zero);
      throw StateError('handler failed');
    };
    await challenge(1);
    expect(calls.last.arguments, {'id': 1});
  });

  test('late responses after disposal do not call the native instance',
      () async {
    await challenge(1);
    await controller.dispose();
    await requests.single.onProceed(
        const WebviewHttpAuthCredential(user: 'late', password: 'late'));
    expect(calls.where((call) => call.method == 'completeHttpAuth'), isEmpty);
    controller = WebviewController();
    await controller.initialize();
  });

  test('credential submission reports native failures', () async {
    await challenge(1);
    messenger.setMockMethodCallHandler(channel, (call) async {
      throw PlatformException(code: 'method_failed');
    });
    await expectLater(
        requests.single.onProceed(
            const WebviewHttpAuthCredential(user: 'foo', password: 'bar')),
        throwsA(isA<PlatformException>()));
  });

  final realms = <String, String?>{
    'Basic': null,
    'Basic realm=""': '',
    'Basic realm="   "': '   ',
    'Basic realm="Prototype"': 'Prototype',
    'Basic REALM = "Prototype", charset="UTF-8"': 'Prototype',
    'Digest realm="Prototype, private", nonce="123", qop="auth"':
        'Prototype, private',
    r'Basic realm="A \"quoted\" \\ realm"': 'A "quoted" \\ realm',
    'Digest nonce="not a realm=wrong", realm="correct"': 'correct',
    'Basic realm=Prototype': 'Prototype',
    'Digest nonce="123",\trealm="Prototype"': 'Prototype',
    'Digest nonce="123", \t realm="Prototype"': 'Prototype',
    'Digest nonce="123",\tReAlM\t=\t"Prototype"': 'Prototype',
    'Basic realm="\u65e5\u672c\u8a9e"': '\u65e5\u672c\u8a9e',
  };
  for (final entry in realms.entries) {
    test('extracts the realm from ${entry.key}', () async {
      await challenge(1, value: entry.key);
      expect(requests.single.challenge, entry.key);
      expect(requests.single.realm, entry.value);
    });
  }
}
