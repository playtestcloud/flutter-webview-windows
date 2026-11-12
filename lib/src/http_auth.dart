class WebviewHttpAuthCredential {
  const WebviewHttpAuthCredential({required this.user, required this.password});

  final String user;
  final String password;
}

class WebviewHttpAuthRequest {
  const WebviewHttpAuthRequest({
    required this.uri,
    required this.challenge,
    required this.onProceed,
    required this.onCancel,
  });

  final Uri uri;
  final String challenge;

  /// Only the first response takes effect; responses after disposal are ignored.
  final Future<void> Function(WebviewHttpAuthCredential) onProceed;
  final Future<void> Function() onCancel;

  String get host => uri.host;

  String? get realm {
    final parameters = RegExp(
      r"""(?:^|[ ,\t])([!#$%&'*+.^_`|~0-9a-z-]+)\s*=\s*(?:"((?:[^"\\]|\\.)*)"|([^\s,]+))""",
      caseSensitive: false,
    );
    for (final match in parameters.allMatches(challenge)) {
      if (match.group(1)!.toLowerCase() == 'realm') {
        return match.group(2)?.replaceAllMapped(
                RegExp(r'\\(.)'), (match) => match.group(1)!) ??
            match.group(3);
      }
    }
    return null;
  }
}
