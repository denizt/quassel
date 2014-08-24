/*
 * Copyright (C) 2008 Remko Troncon
 */

#ifndef SPARKLEAUTOUPDATER_H
#define SPARKLEAUTOUPDATER_H

#include <QString>

#include "autoupdater.h"

class SparkleAutoUpdater : public AutoUpdater {
 public:
  SparkleAutoUpdater();
  ~SparkleAutoUpdater();

  void checkForUpdates();
  void checkForUpdatesInBackground();

 private:
  class Private;
  Private* d;
};

#endif
