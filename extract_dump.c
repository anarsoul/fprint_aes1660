#include <stdio.h>
#include <stdlib.h>

struct frame_header {
	unsigned char type;
	unsigned char size_lsb;
	unsigned char size_msb;
};

int main(int argc, char *argv[])
{
	FILE *in, *out;
	unsigned char buf[0x0244];
	char name[256];
	int frame_size, frame_num = 0, x, y;
	struct frame_header header;

	if (argc != 2) {
		printf("Usage: %s filename\n", argv[0]);
		return 0;
	}

	in = fopen(argv[1], "rb");

	while (!feof(in)) {
		if (!fread(&header, sizeof(header), 1, in))
			break;
		frame_size = header.size_msb * 256 + header.size_lsb;
		if (header.type != 0x49) {
			printf("header type is %.2x\n", (int)header.type);
			fseek(in, frame_size, SEEK_CUR);
			continue;
		}
		if (frame_size != 0x244) {
			printf("Bogus frame size: %.4x\n", frame_size);
			printf("file offset is %.8x\n", (int)ftell(in));
		}
		if (!fread(buf, frame_size, 1, in))
			break;
		printf("%.2x %.2x\n", (int)buf[0], (int)buf[1]);
		if (frame_size != 0x244)
			continue;
		snprintf(name, sizeof(name), "frame-%.5d.pnm", frame_num);
		out = fopen(name, "wb");
		fprintf(out, "P2\n");
		fprintf(out, "8 128\n");
		fprintf(out, "15\n");
		for (y = 0; y < 192; y++) {
			for (x = 0; x < 4; x++) {
				fprintf(out, "%.2d %.2d ", (int)(buf[40 + y * 4 + x] >> 4), (int)(buf[40 + y * 4 + x] & 0xf));
			}
			fprintf(out, "\n");
		}
		fclose(out);
		frame_num++;
	}
	printf("Got %d frames!\n", frame_num);
}
