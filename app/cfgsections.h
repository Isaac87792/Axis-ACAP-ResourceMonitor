/* 1.7.0 Copy settings to other cameras: adds Resource Monitor's sections (settings, alerts, sendout) to the
 * configio endpoint. Call once at start, after kit_configio_init() and alerts_init(). */
#ifndef CFGSECTIONS_H
#define CFGSECTIONS_H
void cfgsections_init(void);
#endif
