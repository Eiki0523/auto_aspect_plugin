# Auto Aspect Plugin for AviUtl2

自動でシーン解像度を素材のサイズに合わせる AviUtl2 用の汎用プラグインです。

## 機能

- 新規プロジェクトで最初に読み込んだ動画または画像ファイルを検知
- 素材ファイルの実サイズを取得し、シーンの解像度を自動で同じ値に変更
- すでに同じ解像度になっている場合は変更せずログに通知
- 1プロジェクトにつき最初の1回だけ実行し、その後は手動操作を妨げません

## ビルド方法

このプラグインは 64bit 版 AviUtl2 を対象にしています。Visual Studio 2022 などの MSVC ツールチェーンで CMake プロジェクトとしてビルドできます。

1. 任意のビルド用ディレクトリを作成します。  
   例: `mkdir build && cd build`
2. 64bit 向けに CMake を生成します。  
   `cmake -G "Visual Studio 17 2022" -A x64 ..`
3. 生成されたソリューションまたは `cmake --build . --config Release` でビルドします。
4. 出力物 `auto_aspect.aux2` を AviUtl2 のプラグインフォルダに配置します。

> **メモ:** Media Foundation と Windows Imaging Component を利用するため、Windows 10 以降を前提としています。

## フォルダ構成

- `CMakeLists.txt` : CMake プロジェクトファイル
- `include/aviutl2_sdk/plugin2.h` : AviUtl2 SDK ヘッダー (ミラーから取得済み)
- `src/auto_aspect.cpp` : プラグイン本体

## ライセンス

AviUtl2 SDK は MIT License ベースです。本コードも同じく MIT License とします。
