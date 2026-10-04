/** @fileoverview Runs the upload smoke tests: the .sxcu format and jobs, every
 *  provider against a local server, and hosts configured in omasnap.conf.
 *  Nothing reaches a real host, keyring or browser. */
#include <QApplication>
#include <QStandardPaths>

int runUploadSxcuSmoke(int argc, char **argv);
int runUploadProviderSmoke(int argc, char **argv);
int runUploadConfigSmoke(int argc, char **argv);

int main(int argc, char **argv) {
  // Secrets go to a file under the test locations, never the real keyring.
  QStandardPaths::setTestModeEnabled(true);
  QApplication application(argc, argv);
  int failures = 0;
  failures += runUploadSxcuSmoke(argc, argv);
  failures += runUploadProviderSmoke(argc, argv);
  failures += runUploadConfigSmoke(argc, argv);
  return failures == 0 ? 0 : 1;
}
