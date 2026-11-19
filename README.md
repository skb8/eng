# eng

Минимальный Android-движок на C++17. `SurfaceView` передаёт поверхность через JNI, а C++ получает `ANativeWindow` и создаёт Vulkan surface и swapchain. Render pass заливает экран синим цветом и выводит кадр. Для статической заливки кадр рисуется по запросу Android. При изменении размера или пересоздании поверхности ресурсы создаются заново.

Нужны Android 7.0 (API 24) или новее, Vulkan 1.0.3 и ABI `arm64-v8a` или `x86_64`. Цвет задаётся в `Renderer::draw()` в файле `app/src/main/cpp/renderer.cpp`.

## Сборка и установка

Нужны JDK 17 и Android SDK. Установите зависимости через SDK Manager в Android Studio или с помощью `sdkmanager`.

```sh
sdkmanager "platforms;android-35" "build-tools;34.0.0" "ndk;28.2.13676358" "cmake;3.22.1"
./gradlew :app:assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n io.github.skb8.eng/.MainActivity
```

В [GitHub Actions](https://github.com/skb8/eng/actions) workflow **Android APK** запускается на push, pull request или вручную. APK доступен в артефакте `eng-debug`.
