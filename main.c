
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <fcntl.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

/* should probably not hardcode this... */
#define DRI_PATH "/dev/dri/card0"

#define MAX_ITER 500
#define TRUNCATE(a) (255 * ((double) (a)/MAX_ITER))
#define MAKE_WHITE(color) (color + (color << 8) + (color << 16))

struct screen {
	drmModeCrtc *saved_crtc;
	drmModeModeInfo mode;
	uint32_t *buf;
	uint32_t width, height;
	uint32_t conn_id, enc_id, crtc_id, fb_id;
	uint32_t handle, pitch;
	uint64_t size;
};

static struct screen *get_screen(int fd)
{
	struct screen *scr;
	drmModeRes *res;
	drmModeConnector *conn;
	drmModeEncoder *enc;
	int ret = 0;

	res = drmModeGetResources(fd);
	if(!res)
		return NULL;

	conn = drmModeGetConnector(fd, res->connectors[0]);
	if(!conn) {
		ret = 1;
		goto out;
	}

	scr = malloc(sizeof(*scr));
	memcpy(&scr->mode, &conn->modes[0], sizeof(drmModeModeInfo));
	scr->width = conn->modes[0].hdisplay;
	scr->height = conn->modes[0].vdisplay;
	scr->conn_id = conn->connector_id;
	scr->enc_id = conn->encoder_id;

	enc = drmModeGetEncoder(fd, conn->encoder_id);
	if(!enc) {
		ret = 1;
		goto out;
	}

	scr->crtc_id = enc->crtc_id;

	drmModeFreeEncoder(enc);
out:
	drmModeFreeConnector(conn);
	drmModeFreeResources(res);
	if(ret)
		return NULL;

	return scr;
}

static int setup_dumb_fb(int fd, struct screen *scr)
{
	int ret;
	uint64_t offset;

	ret = drmModeCreateDumbBuffer(fd,
				      scr->width,
				      scr->height,
				      32,
				      0,
				      &scr->handle, &scr->pitch, &scr->size);
	if(ret)
		return EXIT_FAILURE;

	ret = drmModeAddFB(fd,
			   scr->width,
			   scr->height,
			   24,
			   32,
			   scr->pitch, scr->handle, &scr->fb_id);
	if(ret)
		goto out;

	ret = drmModeMapDumbBuffer(fd, scr->handle, &offset);
	if(ret)
		goto out;

	scr->buf = mmap(NULL, scr->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset);
	scr->saved_crtc = drmModeGetCrtc(fd, scr->crtc_id);
	if(!scr->saved_crtc)
		goto out_crtc;

	/* this requires DRM master, and will return an error if not so */
	ret = drmModeSetCrtc(fd,
			     scr->crtc_id,
			     scr->fb_id,
			     0,
			     0,
			     &scr->conn_id,
			     1,
			     &scr->mode);
	if(ret)
		goto out_crtc;

	return EXIT_SUCCESS;
out_crtc:
	munmap(scr->buf, scr->size);
out:
	drmModeDestroyDumbBuffer(fd, scr->handle);
	return EXIT_FAILURE;
}

static void free_screen(struct screen *scr)
{
	if(!scr)
		return;
	free(scr);
}

static int mb_iterate(double x, double y, unsigned int max_iter)
{
	unsigned int iter;
	double r = x;
	double i = y;
	for (iter = 0; iter < max_iter; ++iter) {
		double r2 = r*r, i2 = i*i;
		if(r2+i2 >= 4.0)
			break;
		double ri = r*i;
		i = ri+ri + y;
		r = r2-i2 + x;
	}
	return iter;
}

static void draw(struct screen *scr)
{
	const double ratio = ((double) scr->width)/((double) scr->height);
	double cre = -1.4;
	double cim = 0;
	double diam = 0.2;
	double minr, mini, maxr, maxi;
	double stepr, stepi;
	uint32_t iter;
	uint32_t i, j;

	const double zoom = 0.05;

	minr = cre - diam * zoom * ratio;
        mini = cim - diam * zoom;
        maxr = cre + diam * zoom * ratio;
        maxi = cim + diam * zoom;

        stepr = (maxr - minr) / scr->width;
        stepi = (maxi - mini) / scr->height;

	for(i = 0; i < scr->height; i++) {
		for(j = 0; j < scr->width; j++) {
			iter = TRUNCATE(mb_iterate(minr + j*stepr, mini + i*stepi, MAX_ITER));
			*(scr->buf + j + (i*scr->width)) = MAKE_WHITE(iter);
		}
	}
}


static int destroy_fb(int fd, struct screen *scr)
{
	int ret;
	ret = drmModeSetCrtc(fd, scr->saved_crtc->crtc_id,
			     scr->saved_crtc->buffer_id, scr->saved_crtc->x,
			     scr->saved_crtc->y, &scr->conn_id, 1,
			     &scr->saved_crtc->mode);
	if(ret)
		return EXIT_FAILURE;

	drmModeFreeCrtc(scr->saved_crtc);
	scr->saved_crtc = NULL;

	munmap(scr->buf, scr->size);
	ret = drmModeRmFB(fd, scr->fb_id);
	if(ret) {
		fprintf(stderr, "Failed to remove framebuffer!");
		return EXIT_FAILURE;
	}
	drmModeDestroyDumbBuffer(fd, scr->handle);

	return 0;
}


int main(void)
{
	int fd, ret;
	uint64_t has_dumb;
	struct screen *scr;

	/*
	 * First, we need to open the DRM device node. Usually this should not
	 * fail.
	 */
	fd = open(DRI_PATH, O_RDWR);
	if(fd == -1) {
		perror("Failed to open dri device");
		return EXIT_FAILURE;
	}

	/*
	 * Check if the DRM device has the ability to have dumb framebuffers
	 * attached to it. We need this for drawing stuff to the screen.
	 */
	drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &has_dumb);
	if(!has_dumb) {
		fprintf(stderr, "dri device does not have dumb framebuffer capability");
		return EXIT_FAILURE;
	}

	/* This is just a convenience function for me */
	scr = get_screen(fd);
	if(!scr) {
		fprintf(stderr, "Failed to get screen!\n");
		return EXIT_FAILURE;
	}

	printf("Screen info:\n"
	       "width: %d height: %d\n", scr->width, scr->height);

	/*
	 * In order to change the CRTC to scan out our framebuffer (next step),
	 * we need to have DRM master status, and this function will fail if
	 * another process has that privilege.
	 */
	ret = drmSetMaster(fd);
	if(ret) {
		fprintf(stderr, "Failed to set DRM master!\n");
		return EXIT_FAILURE;
	}

	/*
	 * Now we create a dumb framebuffer in GPU memory, and then map it to
	 * our process's address space.
	 */
	ret = setup_dumb_fb(fd, scr);
	if(ret) {
		fprintf(stderr, "Failed to setup dumb framebuffer!\n");
		return EXIT_FAILURE;
	}

	/* This is the actual draw function. */
	draw(scr);

	/* Press any key to continue */
	getchar();

	/* Now we tear down our created framebuffer. */
	ret = destroy_fb(fd, scr);
	if(ret) {
		fprintf(stderr, "Failed to destroy framebuffer!\n");
		return EXIT_FAILURE;
	}

	/*
	 * Since we don't need modesettings capabilities no more, we can drop
	 * DRM master status.
	 */
	drmDropMaster(fd);

	/* We don't need this struct anymore so we can free it. */
	free_screen(scr);

	/* And close the file descriptor. */
	close(fd);

	return EXIT_SUCCESS;
}
