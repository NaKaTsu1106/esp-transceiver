#pragma once

/**
 * LTE TX/RX 往復試験
 *
 * 音声送信と同等のデータ量（50B/20ms）のテストパケットを
 * 実際の LTE → VPS サーバー → LTE パスで往復させ、
 * 受信側でパターン検証・パケットロス集計を行う。
 *
 * 使い方:
 *   1. main.c の TEST_LTE_ENABLE を 1 にしてビルド・フラッシュ
 *   2. サーバー側で echo モードを有効にする（audio_server.py の LTE_TEST_ECHO=True）
 *   3. シリアルモニターで LTE-TEST PASS/FAIL/LOSS ログを確認する
 */
void lte_test_run(void);
