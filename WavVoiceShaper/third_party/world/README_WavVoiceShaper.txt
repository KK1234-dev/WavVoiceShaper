WavVoiceShaper WORLD同梱メモ
================================

このフォルダには、WavVoiceShaper の WORLD エンジンで使用する公式 WORLD の
src フォルダとライセンス通知を同梱しています。

最終配置:
  WavVoiceShaper\WavVoiceShaper\third_party\world\src\world\dio.h
  WavVoiceShaper\WavVoiceShaper\third_party\world\src\world\cheaptrick.h
  WavVoiceShaper\WavVoiceShaper\third_party\world\src\dio.cpp
  WavVoiceShaper\WavVoiceShaper\third_party\world\src\cheaptrick.cpp
  ...

この階層に src が存在する場合、WavVoiceShaper.cpp の __has_include 判定で
WORLD 処理が有効になります。src が無い場合は Builtin 処理に自動フォールバックします。

取得元:
  https://github.com/mmorise/World

ライセンス:
  WORLD は BSD 3-Clause License です。ソースまたはバイナリを再配布する場合は、
  このディレクトリの LICENSE.txt にある著作権表示・条件・免責条項を保持してください。
