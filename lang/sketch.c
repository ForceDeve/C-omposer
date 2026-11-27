#include <stdio.h>
#include <gtk-4.0/gtk/gtk.h>
typedef struct {
	//to be determined how to handle tone
	// type tone
	// type distortion
	// type eq
	// type etc
}tone_t;
typedef struct {
	tone_t tone;
	char pitch;
	char volume;
}note;
typedef struct {
	tone_t tone; // will have to implement inheritance so you can play leads and not just chords
	note *notes;
	char mode;
	char scale;
}scale;
int main(void) {


	return 0;
}
