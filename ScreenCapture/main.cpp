#include <stdio.h>
#include "ScreenCapture.h"

/* https://qiita.com/i_saint/items/ad5b0545873d0cff4604 */
/* https://github.com/i-saint/ScreenCaptureTest */

int main(int argc, char *argv) {
  int _loop = 1;
  do {
    printf_s("[1] shot\n[0] exit\nCommand: ");
    int _cmd;
    scanf_s("%d", &_cmd);
    if (_cmd == 0) {
      _loop = 0;
    } else if (_cmd == 1) {
      ScreenCapture("cap.jpg");
    }
  } while (_loop == 1);
  return 0;
}
