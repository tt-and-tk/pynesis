// 意味解析エラー: 関数呼び出しネストが最大段数(10段)を超える (異常系)
void f11(void) { }
void f10(void) { f11(); }
void f9(void)  { f10(); }
void f8(void)  { f9(); }
void f7(void)  { f8(); }
void f6(void)  { f7(); }
void f5(void)  { f6(); }
void f4(void)  { f5(); }
void f3(void)  { f4(); }
void f2(void)  { f3(); }
void f1(void)  { f2(); }
void main(void) {
    f1();
}
