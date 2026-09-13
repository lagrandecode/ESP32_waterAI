// Firebase configuration for the Water AI project (esp32-waterai).
// Generated from `firebase apps:sdkconfig` output.
import 'dart:io' show Platform;

import 'package:firebase_core/firebase_core.dart' show FirebaseOptions;

class DefaultFirebaseOptions {
  static FirebaseOptions get currentPlatform {
    if (Platform.isIOS) return ios;
    return android;
  }

  static const FirebaseOptions ios = FirebaseOptions(
    apiKey: 'AIzaSyCaxbZDSuO9m1rqNlSBFV0v4kKl36GAjhk',
    appId: '1:691421834578:ios:d9721abd61ce96eb25de8b',
    messagingSenderId: '691421834578',
    projectId: 'esp32-waterai',
    storageBucket: 'esp32-waterai.firebasestorage.app',
    iosBundleId: 'com.seun.waterai',
  );

  static const FirebaseOptions android = FirebaseOptions(
    apiKey: 'AIzaSyBk771ziKYX6rPtqx6yB9lZag7O71BBPWM',
    appId: '1:691421834578:android:b5878eab99fb145b25de8b',
    messagingSenderId: '691421834578',
    projectId: 'esp32-waterai',
    storageBucket: 'esp32-waterai.firebasestorage.app',
  );
}
