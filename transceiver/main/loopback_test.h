#pragma once

/**
 * マイク → Opus エンコード → Opus デコード → スピーカー
 * ローカルループバックテスト。
 *
 * 使い方: main.c の TEST_LOOPBACK_ENABLE を 1 にしてビルド・フラッシュする。
 * LTE 接続・サーバーなしで音声パイプラインを単独検証できる。
 */
void loopback_run(void);
