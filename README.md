# eng

Минимальный Android-движок на C++17 и Vulkan. Приложение рисует разноцветный куб с буфером глубины. Камера вращается вокруг куба автоматически. Перетаскивание пальцем меняет угол и наклон камеры. HUD показывает FPS, среднее время кадра и размер swapchain.

Java передаёт через JNI только `Surface`, её размеры и касания. Игровой цикл, ввод и Vulkan работают в отдельном `std::thread`. Поток ждёт, пока Android предоставит поверхность. При её уничтожении поток освобождает оконные ресурсы до возврата из `surfaceDestroyed`. `VulkanContext` хранит `VkInstance`, `VkDevice`, VMA и ресурсы сцены. `Swapchain` пересоздаётся при смене поверхности, её размера или по сигналу Vulkan.

Математику камеры выполняет GLM. VMA выделяет память для вершинных буферов и буфера глубины. CMake получает закреплённые версии GLM и VMA при первой сборке и компилирует `shaders/scene.vert` и `shaders/scene.frag` в SPIR-V с помощью `glslc` из NDK.

## Сборка и установка

Нужны JDK 17 и Android SDK. Установите зависимости через SDK Manager в Android Studio или с помощью `sdkmanager`. Для запуска нужны Android 7.0 (API 24) или новее, Vulkan 1.0.3 и ABI `arm64-v8a` или `x86_64`.

```sh
sdkmanager "platforms;android-35" "build-tools;34.0.0" "ndk;28.2.13676358" "cmake;3.22.1"
./gradlew :app:assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n io.github.skb8.eng/.MainActivity
```

В [GitHub Actions](https://github.com/skb8/eng/actions) workflow **Android APK** запускается на push, pull request или вручную. APK доступен в артефакте `eng-debug`.
