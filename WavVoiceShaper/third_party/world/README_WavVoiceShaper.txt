WavVoiceShaper WORLD配置メモ
================================

このフォルダに公式WORLDの src フォルダを配置してください。

最終配置:
  WavVoiceShaper\WavVoiceShaper\third_party\world\src\world\dio.h
  WavVoiceShaper\WavVoiceShaper\third_party\world\src\world\cheaptrick.h
  WavVoiceShaper\WavVoiceShaper\third_party\world\src\dio.cpp
  WavVoiceShaper\WavVoiceShaper\third_party\world\src\cheaptrick.cpp
  ...

この階層にsrcが存在する場合、WavVoiceShaper.cppの __has_include 判定で
WORLD処理が有効になります。srcが無い場合は従来のBuiltin処理に自動フォールバックします。

取得元:
  https://github.com/mmorise/World

ライセンス:
  WORLDはmodified-BSD license表記です。納品・配布する場合は、公式リポジトリのLICENSEを
  third_party\world\LICENSE として同梱してください。
