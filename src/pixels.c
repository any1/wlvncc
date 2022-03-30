#include <wayland-client.h>
#include <pixman.h>
#include <stdbool.h>

bool wl_shm_to_pixman_fmt(pixman_format_code_t* dst, enum wl_shm_format src)
{
#define LOWER_R r
#define LOWER_G g
#define LOWER_B b
#define LOWER_A a
#define LOWER_X x
#define LOWER_
#define LOWER(x) LOWER_##x

#define CONCAT_(a, b) a ## b
#define CONCAT(a, b) CONCAT_(a, b)

#define FMT_WL_SHM(x, y, z, v, a, b, c, d) WL_SHM_FORMAT_##x##y##z##v##a##b##c##d

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define FMT_PIXMAN(x, y, z, v, a, b, c, d) \
	CONCAT(CONCAT(CONCAT(CONCAT(CONCAT(CONCAT(CONCAT(CONCAT(\
	PIXMAN_, LOWER(x)), a), LOWER(y)), b), LOWER(z)), c), LOWER(v)), d)
#else
#define FMT_PIXMAN(x, y, z, v, a, b, c, d) \
	CONCAT(CONCAT(CONCAT(CONCAT(CONCAT(CONCAT(CONCAT(CONCAT(\
	PIXMAN_, LOWER(v)), d), LOWER(z)), c), LOWER(y)), b), LOWER(x)), a)
#endif

	switch (src) {
#define X(...) \
	case FMT_WL_SHM(__VA_ARGS__): *dst = FMT_PIXMAN(__VA_ARGS__); break

	/* 32 bits */
	X(A,R,G,B,8,8,8,8);
	X(A,B,G,R,8,8,8,8);
	X(X,R,G,B,8,8,8,8);
	X(X,B,G,R,8,8,8,8);
	X(R,G,B,A,8,8,8,8);
	X(B,G,R,A,8,8,8,8);
	X(R,G,B,X,8,8,8,8);
	X(B,G,R,X,8,8,8,8);

	/* 24 bits */
	X(R,G,B,,8,8,8,);
	X(B,G,R,,8,8,8,);

	/* 16 bits */
	X(R,G,B,,5,6,5,);
	X(B,G,R,,5,6,5,);

	/* These are incompatible on big endian */
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	X(A,R,G,B,2,10,10,10);
	X(X,R,G,B,2,10,10,10);
	X(A,B,G,R,2,10,10,10);
	X(X,B,G,R,2,10,10,10);
	X(A,R,G,B,1,5,5,5);
	X(A,B,G,R,1,5,5,5);
	X(X,R,G,B,1,5,5,5);
	X(X,B,G,R,1,5,5,5);
	X(A,R,G,B,4,4,4,4);
	X(A,B,G,R,4,4,4,4);
	X(X,R,G,B,4,4,4,4);
	X(X,B,G,R,4,4,4,4);
#endif

#undef X

	default: return false;
	}

	return true;
}
