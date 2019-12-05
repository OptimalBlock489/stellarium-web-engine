/* Stellarium Web Engine - Copyright (c) 2018 - Noctua Software Ltd
 *
 * This program is licensed under the terms of the GNU AGPL v3, or
 * alternatively under a commercial licence.
 *
 * The terms of the AGPL v3 license can be found in the main directory of this
 * repository.
 */

#include "swe.h"
#include "ini.h"
#include <regex.h>
#include <zlib.h> // For crc32.

/* Planets module.
 *
 * All the data is in the file data/planets.ini.
 */

// Orbit elements, with ICRF reference PLANE.
typedef struct elements
{
    double mjd;     // date (MJD).
    double in;      // inclination (rad).
    double om;      // Longitude of the Ascending Node (rad).
    double w;       // Argument of Perihelion (rad).
    double a;       // Mean distance (Semi major axis) (AU).
    double n;       // Daily motion (rad/day).
    double ec;      // Eccentricity.
    double ma;      // Mean Anomaly (rad).
} elements_t;

typedef struct planet planet_t;

// The planet object klass.
struct planet {
    obj_t       obj;

    // Constant data.
    const char  *name;
    planet_t    *parent;
    double      radius_m;  // (meter)
    double      albedo;
    double      color[4];
    double      shadow_brightness; // in [0-1].
    int         id; // Uniq id number, as defined in JPL HORIZONS.
    double      mass;       // kg (0 if unknown).

    // Optimizations vars
    float update_delta_s;    // Number of seconds between 2 orbits full update
    double last_full_update; // Time of last full orbit update (TT)
    double last_full_pvh[2][3]; // equ, J2000.0, AU heliocentric pos and speed.

    // Cached pvo value and the observer hash used for the computation.
    uint64_t pvo_obs_hash;
    double pvo[2][3];

    // Rotation elements
    struct {
        double obliquity;   // (rad)
        double period;      // (day)
        double offset;      // (rad)
        double pole_ra;     // (rad)
        double pole_de;     // (rad)
    } rot;

    // Orbit elements (in ICRF plane, relative to the parent body).
    elements_t orbit;

    // Rings attributes.
    struct {
        double inner_radius; // (meter)
        double outer_radius; // (meter)
        texture_t *tex;
    } rings;

    hips_t      *hips;              // Hips survey of the planet.
    hips_t      *hips_normalmap;    // Normal map survey.

    fader_t     orbit_visible;
};

// Planets layer object type;
typedef struct planets {
    obj_t       obj;
    fader_t     visible;
    planet_t    *planets; // The map of all the planets.
    planet_t    *sun;
    planet_t    *earth;

    // Earth shadow on a lunar eclipse.
    texture_t *earth_shadow_tex;
    // Sun halo.
    texture_t *halo_tex;
    // Default HiPS survey.
    hips_t *default_hips;
} planets_t;


/*
 * List of known bodies id.
 * We use them to quickly test for a given planet.  They follow the ids
 * used by JPL HORIZONS service.
 */
enum {
    SUN = 10,
    MERCURY = 199,
    VENUS = 299,
    MOON = 301,
    EARTH = 399,
    MARS = 499,
    IO = 501,
    EUROPA = 502,
    GANYMEDE = 503,
    CALLISTO = 504,
    JUPITER = 599,
    SATURN = 699,
    URANUS = 799,
    NEPTUNE = 899,
};

/* visual elements of planets
 * [planet][0] = angular size at 1 AU
 * [planet][1] = magnitude at 1 AU from sun and earth and 0 deg phase angle
 * [planet][2] = A
 * [planet][3] = B
 * [planet][4] = C
 *   where mag correction = A*(i/100) + B*(i/100)^2 + C*(i/100)^3
 *      i = angle between sun and earth from planet, degrees
 * from Explanatory Supplement, 1992
 */
static const double VIS_ELEMENTS[][5] = {
        /*         */   {},
        /* Mercury */   {6.74, -0.36, 3.8, -2.73, 2.00},
        /* Venus */     {16.92, -4.29, 0.09, 2.39, -.65},
        /* Earth */     {},
        /* Mars */      {9.36, -1.52, 1.60, 0., 0.},
        /* Jupiter */   {196.74, -9.25, 0.50, 0., 0.},
        /* Saturn */    {165.6, -8.88, 4.40, 0., 0.},
        /* Uranus */    {70.481, -7.19, 0.28, 0., 0.},
        /* Neptune */   {68.294, -6.87, 0., 0., 0.},
        /* Pluto */     {8.2, -1.01, 4.1, 0., 0.},
};


#define PLANETS_ITER(o, p) \
    for (   p = (planet_t*)(((obj_t*)o)->children); \
            p; \
            p = (planet_t*)p->obj.next)


/*
 * Function: moon_icrf_geocentric
 * Compute moon position at a given time
 *
 * Parameters:
 *   tt     - TT time in MJD.
 *   pos    - Output position in ICRF frame, geocentric.
 */
static void moon_icrf_geocentric_pos(double tt, double pos[3])
{
    double rmatecl[3][3], rmatp[3][3];
    double lambda, beta, dist, obl;
    // Get ecliptic position of date.
    moon_pos(DJM0 + tt, &lambda, &beta, &dist);
    dist *= 1000.0 / DAU; // km to AU.
    // Convert to equatorial.
    obl = eraObl06(DJM0, tt); // Mean oblicity of ecliptic at J2000.
    eraIr(rmatecl);
    eraRx(-obl, rmatecl);
    eraS2p(lambda, beta, dist, pos);
    eraRxp(rmatecl, pos, pos);
    // Precess back to J2000.
    eraPmat76(DJM0, tt, rmatp);
    eraTrxp(rmatp, pos, pos);
}


/*
 * Function: planet_get_pvh
 * Get the heliocentric (ICRF) position of a planet at a given time.
 */
static void planet_get_pvh(const planet_t *planet, const observer_t *obs,
                           double pvh[2][3])
{
    double dt, parent_pvh[2][3];
    int n;

    // Use cached value if possible.
    if (planet->last_full_update) {
        dt = obs->tt - planet->last_full_update;
        if (fabs(dt) < planet->update_delta_s / ERFA_DAYSEC) {
            eraPvu(dt, planet->last_full_pvh, pvh);
            return;
        }
    }

    switch (planet->id) {
    case EARTH:
        eraCpv(obs->earth_pvh, pvh);
        return;
    case SUN:
        eraZpv(pvh);
        return;
    case MOON:
        moon_icrf_geocentric_pos(obs->tt, pvh[0]);
        moon_icrf_geocentric_pos(obs->tt + 1, pvh[1]);
        vec3_sub(pvh[1], pvh[0], pvh[1]);
        eraPvppv(pvh, obs->earth_pvh, pvh);
        return;

    case MERCURY:
    case VENUS:
    case MARS:
    case JUPITER:
    case SATURN:
    case URANUS:
    case NEPTUNE:
        n = (planet->id - MERCURY) / 100 + 1;
        eraPlan94(DJM0, obs->tt, n, pvh);
        break;

    case IO:
    case EUROPA:
    case GANYMEDE:
    case CALLISTO:
        planet_get_pvh(planet->parent, obs, parent_pvh);
        l12(DJM0, obs->tt, planet->id - IO + 1, pvh);
        vec3_add(pvh[0], parent_pvh[0], pvh[0]);
        vec3_add(pvh[1], parent_pvh[1], pvh[1]);
        break;

    default:
        planet_get_pvh(planet->parent, obs, parent_pvh);
        orbit_compute_pv(0, obs->tt, pvh[0], pvh[1],
                planet->orbit.mjd,
                planet->orbit.in,
                planet->orbit.om,
                planet->orbit.w,
                planet->orbit.a,
                planet->orbit.n,
                planet->orbit.ec,
                planet->orbit.ma,
                0.0, 0.0);
        vec3_add(pvh[0], parent_pvh[0], pvh[0]);
        vec3_add(pvh[1], parent_pvh[1], pvh[1]);
        break;
    }

    // Cache the value for next time.
    eraCpv(pvh, planet->last_full_pvh);
    ((planet_t*)planet)->last_full_update = obs->tt;
}

/*
 * Function: planet_get_pvo
 * Return observed position of a planet (ICRF, centered on observer).
 *
 * Parameters:
 *   adjust_ligth_speed - if set to false, compute the observed position
 *   without light speed adjustement (that is the position if the speed of
 *   light was infinite).
 */
static void planet_get_pvo(const planet_t *planet, const observer_t *obs,
                           double pvo[2][3], bool adjust_light_speed)
{
    double pvh[2][3];
    double ldt;

    // Use cached value if possible.
    if (adjust_light_speed && obs->hash == planet->pvo_obs_hash) {
        eraCpv(planet->pvo, pvo);
        return;
    }

    planet_get_pvh(planet, obs, pvh);
    eraCpv(pvh, pvo);
    eraPvppv(pvo, obs->sun_pvb, pvo);
    eraPvmpv(pvo, obs->obs_pvb, pvo);
    if (!adjust_light_speed) return;

    // Apply light speed adjustment.
    ldt = vec3_norm(pvo[0]) * DAU / LIGHT_YEAR_IN_METER * DJY;
    observer_t obs2 = *obs;
    obs2.tt -= ldt;
    planet_get_pvh(planet, &obs2, pvh);
    eraCpv(pvh, pvo);
    eraPvppv(pvo, obs->sun_pvb, pvo);
    eraPvmpv(pvo, obs->obs_pvb, pvo);

    // Copy value into cache to speed up next access.
    ((planet_t*)planet)->pvo_obs_hash = obs->hash;
    eraCpv(pvo, planet->pvo);
}

// Same as get_pvo, but return homogenous 4d coordinates.
static void planet_get_pvo4(const planet_t *planet, const observer_t *obs,
                            double pvo4[2][4])
{
    double pvo[2][3];
    planet_get_pvo(planet, obs, pvo, true);
    vec3_copy(pvo[0], pvo4[0]);
    vec3_copy(pvo[1], pvo4[1]);
    pvo4[0][3] = 1;
    pvo4[1][3] = 1;
}

/*
 * Compute the illumination from the sun taking into possible eclipses.
 */
static double compute_sun_eclipse_factor(const planet_t *sun,
                                         const observer_t *obs)
{
    // For the moment we assume the observer is always on Earth!
    double sun_r;   // Sun radius as seen from observer.
    double sph_r;   // Blocking body radius as seen from observer.
    double sep;     // Separation angle between sun and body.
    double pvo[2][3];
    planet_t *p;

    sun_r = 2.0 * sun->radius_m / DAU / vec3_norm(obs->sun_pvo[0]);

    PLANETS_ITER(sun->obj.parent, p) {
        if (p->id != MOON) continue; // Only consider the Moon.
        planet_get_pvo(p, obs, pvo, true);
        sph_r = 2.0 * p->radius_m / DAU / vec3_norm(pvo[0]);
        sep = eraSepp(obs->sun_pvo[0], pvo[0]);
        // Compute shadow factor.
        // XXX: this should be in algos.
        if (sep >= sun_r + sph_r) return 1.0; // Outside of shadow.
        if (sep <= sph_r - sun_r) return 0.0; // Umbra.
        if (sep <= sun_r - sph_r) // Penumbra completely inside.
            return 1.0 - sph_r * sph_r / (sun_r * sun_r);
        // Penumbra partially inside.
        double x = (sun_r * sun_r + sep * sep - sph_r * sph_r) / (2.0 * sep);
        double alpha = acos(x / sun_r);
        double beta = acos((sep - x) / sph_r);
        double AR = sun_r * sun_r * (alpha - 0.5 * sin(2.0 * alpha));
        double Ar = sph_r * sph_r * (beta - 0.5 * sin(2.0 * beta));
        double AS = sun_r * sun_r * 2.0 * 1.57079633;
        return 1.0 - (AR + Ar) / AS;
    }
    return 1.0;
}

static double planet_get_phase(const planet_t *planet, const observer_t *obs)
{
    double i;   // Phase angle.
    double pvh[2][3], pvo[2][3];
    if (planet->id == EARTH || planet->id == SUN)
        return NAN;
    planet_get_pvh(planet, obs, pvh);
    planet_get_pvo(planet, obs, pvo, true);
    i = eraSepp(pvh[0], pvo[0]);
    return 0.5 * cos(i) + 0.5;
}

static double sun_get_vmag(const planet_t *sun, const observer_t *obs)
{
    double eclipse_factor;
    double dist_pc; // Distance in parsec.
    // Compute the apparent magnitude for the absolute mag (V: 4.83) and
    // observer's distance
    dist_pc = vec3_norm(obs->earth_pvh[0]) * (M_PI / 648000);
    eclipse_factor = max(compute_sun_eclipse_factor(sun, obs), 0.000128);
    return 4.83 + 5.0 * (log10(dist_pc) - 1.0) - 2.5 * (log10(eclipse_factor));
}

static double moon_get_vmag(const planet_t *moon, const observer_t *obs)
{
    double el, dist;
    double pvh[2][3], pvo[2][3];

    // This is based on the algo of pyephem.
    // XXX: move into 'algos'.
    planet_get_pvh(moon, obs, pvh);
    planet_get_pvo(moon, obs, pvo, true);
    dist = vec3_norm(pvo[0]);
    el = eraSepp(pvo[0], obs->sun_pvo[0]); // Elongation.
    return -12.7 +
        2.5 * (log10(M_PI) - log10(M_PI / 2.0 * (1.0 + 1.e-6 - cos(el)))) +
        5.0 * log10(dist / .0025);
}

// Compute vmag adjustment from rings.
static double rings_vmag(const planet_t *planet, const observer_t *obs)
{
    double hpos[3]; // Heliocentric pos J2000.0
    double hlon, hlat;
    double earth_hlon, earth_hlat;
    double et, st, set;
    double earth_hpos[3];
    double pvh[2][3];

    if (planet->id != SATURN) return 0;
    planet_get_pvh(planet, obs, pvh);
    mat3_mul_vec3(obs->ri2e, pvh[0], hpos);
    mat3_mul_vec3(obs->ri2e, obs->earth_pvh[0], earth_hpos);

    eraC2s(hpos, &hlon, &hlat);
    eraC2s(earth_hpos, &earth_hlon, &earth_hlat);
    satrings(hlat, hlon, vec3_norm(pvh[0]),
             earth_hlon, vec3_norm(obs->earth_pvh[0]),
             obs->ut1 + DJM0, &et, &st);
    set = sin(fabs(et));
    return (-2.60 + 1.25 * set) * set;
}

static double planet_get_vmag(const planet_t *planet, const observer_t *obs)
{
    const double *vis;  // Visual element of planet.
    double rho; // Distance to Earth (AU).
    double rp;  // Distance to Sun (AU).
    double i;   // Phase angle.
    double mag;
    int n;
    double pvh[2][3], pvo[2][3];

    switch (planet->id) {
    case SUN:
        return sun_get_vmag(planet, obs);
    case MOON:
        return moon_get_vmag(planet, obs);

    case MERCURY:
    case VENUS:
    case MARS:
    case JUPITER:
    case SATURN:
    case URANUS:
    case NEPTUNE:
        planet_get_pvh(planet, obs, pvh);
        planet_get_pvo(planet, obs, pvo, true);
        n = (planet->id - MERCURY) / 100 + 1;
        i = eraSepp(pvh[0], pvo[0]);
        // Compute visual magnitude.
        i *= DR2D / 100;
        rho = vec3_norm(pvh[0]);
        rp = vec3_norm(pvo[0]);
        vis = VIS_ELEMENTS[n];
        return vis[1] + 5 * log10(rho * rp) +
               i * (vis[2] + i * (vis[3] + i * vis[4])) +
               rings_vmag(planet, obs);

    default:
        // http://www.physics.sfasu.edu/astro/asteroids/sizemagnitude.html
        planet_get_pvh(planet, obs, pvh);
        planet_get_pvo(planet, obs, pvo, true);
        rho = vec3_norm(pvh[0]);
        rp = vec3_norm(pvo[0]);
        mag = -1.0 / 0.2 * log10(sqrt(planet->albedo) *
                2.0 * planet->radius_m / 1000.0 / 1329.0);
        return mag + 5 * log10(rho * rp);
    }
}

static int planet_get_info(const obj_t *obj, const observer_t *obs, int info,
                           void *out)
{
    planet_t *planet = (planet_t*)obj;
    double pvo[2][3];

    switch (info) {
    case INFO_PVO:
        planet_get_pvo4(planet, obs, out);
        return 0;
    case INFO_VMAG:
        *(double*)out = planet_get_vmag(planet, obs);
        return 0;
    case INFO_PHASE:
        *(double*)out = planet_get_phase(planet, obs);
        return 0;
    case INFO_RADIUS:
        planet_get_pvo(planet, obs, pvo, true);
        *(double*)out = planet->radius_m / DAU / vec3_norm(pvo[0]);
        return 0;
    default:
        return 1;
    }
}

static void planet_get_designations(
    const obj_t *obj, void *user,
    int (*f)(const obj_t *obj, void *user,
             const char *cat, const char *str))
{
    planet_t *planet = (void*)obj;
    f(obj, user, "NAME", planet->name);
}

static int on_render_tile(hips_t *hips, const painter_t *painter_,
                          const double transf[4][4],
                          int order, int pix, int split, int flags, void *user)
{
    planet_t *planet = USER_GET(user, 0);
    int *nb_tot = USER_GET(user, 1);
    int *nb_loaded = USER_GET(user, 2);
    painter_t painter = *painter_;
    texture_t *tex, *normalmap = NULL;
    uv_map_t map;
    double fade, uv[3][3] = MAT3_IDENTITY, normal_uv[3][3] = MAT3_IDENTITY;
    bool loaded;

    (*nb_tot)++;
    flags |= HIPS_LOAD_IN_THREAD;
    tex = hips_get_tile_texture(hips, order, pix, flags, uv, &fade, &loaded);
    if (loaded) (*nb_loaded)++;
    if (planet->hips_normalmap) {
        (*nb_tot)++;
        normalmap = hips_get_tile_texture(planet->hips_normalmap,
                order, pix, flags, normal_uv, NULL, &loaded);
        if (loaded) (*nb_loaded)++;
    }

    // Texture not ready yet, we just use the planet color.
    if (!tex) {
        vec3_copy(planet->color, painter.color);
        painter.color[3] = 1;
    }
    painter.color[3] *= fade;
    if (planet->id == MOON) painter.flags |= PAINTER_IS_MOON;

    // Hardcoded increase of the luminosity of the moon for the moment!
    // This should be specified in the survey itsefl I guess.
    if (planet->id == MOON)
        vec3_mul(3.8, painter.color, painter.color);

    painter_set_texture(&painter, PAINTER_TEX_COLOR, tex, uv);
    painter_set_texture(&painter, PAINTER_TEX_NORMAL, normalmap, normal_uv);
    uv_map_init_healpix(&map, order, pix, true, false);
    map.transf = (void*)transf;
    paint_quad(&painter, FRAME_ICRF, &map, split);
    return 0;
}

static void ring_project(const uv_map_t *map, const double v[2], double out[4])
{
    const double *radii = map->user;
    double theta, r, mat[3][3], p[4] = {1, 0, 0, 1};
    theta = v[0] * 2 * M_PI;
    r = mix(radii[0], radii[1], v[1]);
    mat3_set_identity(mat);
    mat3_rz(theta, mat, mat);
    mat3_iscale(mat, r, r, 1.0);
    mat3_mul_vec3(mat, p, p);
    vec4_copy(p, out);
}

static void render_rings(const planet_t *planet,
                         const painter_t *painter_,
                         const double transf[4][4])
{
    texture_t *tex = planet->rings.tex;
    double inner_radius = planet->rings.inner_radius / planet->radius_m;
    double outer_radius = planet->rings.outer_radius / planet->radius_m;
    uv_map_t map = {
        .map = ring_project,
        .transf = (void*)transf,
    };
    painter_t painter = *painter_;
    const double radii[2] = {inner_radius, outer_radius};
    double pvo[2][3];

    // Add the planet in the painter shadow candidates.
    if (painter.planet.shadow_spheres_nb < 4) {
        planet_get_pvo(planet, painter.obs, pvo, true);
        vec3_copy(pvo[0],
                  painter.planet.shadow_spheres[
                                    painter.planet.shadow_spheres_nb]);
        painter.planet.shadow_spheres[painter.planet.shadow_spheres_nb][3] =
                  planet->radius_m / DAU;
        painter.planet.shadow_spheres_nb++;
    }

    map.user = radii;
    painter.planet.light_emit = NULL;
    painter.flags &= ~PAINTER_PLANET_SHADER;
    painter.flags |= PAINTER_RING_SHADER;
    painter_set_texture(&painter, PAINTER_TEX_COLOR, tex, NULL);
    paint_quad(&painter, FRAME_ICRF, &map, 64);
}

/*
 * Test if a planet A could cast shadow on a planet B.
 * if a is NULL, then we return false if we know for sure that no body
 * could cast a shadow on b.
 */
static bool could_cast_shadow(const planet_t *a, const planet_t *b,
                              const observer_t *obs)
{
    // Not sure about this algo, I took it pretty as it is from Stellarium.
    const double SUN_RADIUS = 695508000.0 / DAU;
    double pp[3];
    double shadow_dist, d, penumbra_r;
    double apvh[2][3], bpvh[2][3];

    // For the moment we only consider the Jupiter major moons or the
    // Earth on the Moon.
    if (a == NULL)
        return b->id == MOON || (b->id >= IO && b->id <= JUPITER);
    if (b->id == a->id) return false; // No self shadow.
    if ((b->id >= IO && b->id <= JUPITER) && (a->id < IO || a->id > JUPITER))
            return false;
    if (b->id == MOON && a->id != EARTH) return false;

    planet_get_pvh(a, obs, apvh);
    planet_get_pvh(b, obs, bpvh);
    if (vec3_norm2(apvh[0]) > vec3_norm2(bpvh[0])) return false;
    vec3_normalize(apvh[0], pp);
    shadow_dist = vec3_dot(pp, bpvh[0]);
    d = vec2_norm(apvh[0]) / (a->radius_m / DAU / SUN_RADIUS + 1.0);
    penumbra_r = (shadow_dist - d) / d * SUN_RADIUS;
    vec3_mul(shadow_dist, pp, pp);
    vec3_sub(pp, bpvh[0], pp);
    return (vec3_norm(pp) < penumbra_r + b->radius_m / DAU);
}

static int sort_shadow_cmp(const void *a, const void *b)
{
    return -cmp(((const double*)a)[3], ((const double*)b)[3]);
}

/*
 * Compute the list of potential shadow spheres that should be considered
 * when rendering a planet.
 *
 * The returned spheres are xyz = position (in view frame) and w = radius (AU).
 * Sorted with the biggest first.
 *
 * Return the number of candidates.
 */
static int get_shadow_candidates(const planet_t *planet,
                                 const observer_t *obs,
                                 int nb_max,
                                 double (*spheres)[4])
{
    int nb = 0;
    planets_t *planets = (planets_t*)planet->obj.parent;
    planet_t *other;
    double pvo[2][3];

    if (!could_cast_shadow(NULL, planet, obs)) return 0;

    PLANETS_ITER(planets, other) {
        if (could_cast_shadow(other, planet, obs)) {
            // No more space: replace the smallest one in the list if
            // we can.
            if (nb >= nb_max) {
                if (other->radius_m / DAU < spheres[nb_max - 1][3])
                    continue;
                nb--; // Remove the last one.
            }
            planet_get_pvo(other, obs, pvo, true);
            vec3_copy(pvo[0], spheres[nb]);
            spheres[nb][3] = other->radius_m / DAU;
            nb++;
            qsort(spheres, nb, 4 * sizeof(double), sort_shadow_cmp);
        }
    }
    return nb;
}

/*
 * Compute the rotation of a planet along its axis.
 *
 * Parameters:
 *   planet     - A planet.
 *   tt         - TT time (MJD).
 *
 * Return:
 *   The rotation angle in radian.
 */
static double planet_get_rotation(const planet_t *planet, double tt)
{
    if (!planet->rot.period) return 0;
    return (tt - DJM00) / planet->rot.period * 2 * M_PI + planet->rot.offset;
}

static void planet_render_hips(const planet_t *planet,
                               const hips_t *hips,
                               double radius,
                               double r_scale,
                               double alpha,
                               const painter_t *painter_)
{
    // XXX: cleanup this function.  It is getting too big.
    double mat[4][4];
    double tmp_mat[4][4];
    double dist;
    double full_emit[3] = {1.0, 1.0, 1.0};
    double pvo[2][3];
    double angle;
    int nb_tot = 0, nb_loaded = 0;
    double sun_pos[4] = {0, 0, 0, 1};
    planets_t *planets = (planets_t*)planet->obj.parent;
    painter_t painter = *painter_;
    double depth_range[2];
    double shadow_spheres[4][4];
    double pixel_size;
    int split_order;

    if (!hips) hips = planet->hips;
    assert(hips);

    planet_get_pvo(planet, painter.obs, pvo, true);
    angle = 2 * radius * r_scale / vec2_norm(pvo[0]);

    memset(&painter.planet, 0, sizeof(painter.planet));
    // Get potential shadow casting spheres.
    painter.planet.shadow_spheres_nb =
        get_shadow_candidates(planet, painter.obs, 4, shadow_spheres);
    painter.planet.shadow_spheres = shadow_spheres;

    painter.color[3] *= alpha;
    painter.flags |= PAINTER_PLANET_SHADER;

    mat4_set_identity(mat);
    mat4_itranslate(mat, pvo[0][0], pvo[0][1], pvo[0][2]);
    mat4_iscale(mat, radius * r_scale, radius * r_scale, radius * r_scale);
    painter.planet.scale = r_scale;

    // Compute sun position.
    vec3_copy(painter.obs->sun_pvo[0], sun_pos);
    sun_pos[3] = planets->sun->radius_m / DAU;
    painter.planet.sun = &sun_pos;

    // Apply the rotation.
    // Use pole ra/de position if available, else try with obliquity.
    // XXX: Probably need to remove obliquity.
    if (planet->rot.pole_ra || planet->rot.pole_de) {
        mat4_rz(planet->rot.pole_ra, mat, mat);
        mat4_ry(M_PI / 2 - planet->rot.pole_de, mat, mat);
    } else {
        mat3_to_mat4(painter.obs->re2i, tmp_mat);
        mat4_mul(mat, tmp_mat, mat);
        mat4_rx(-planet->rot.obliquity, mat, mat);
    }
    mat4_rz(planet_get_rotation(planet, painter.obs->tt), mat, mat);

    if (planet->id == SUN)
        painter.planet.light_emit = &full_emit;
    if (planet->id == MOON)
        painter.planet.shadow_color_tex = planets->earth_shadow_tex;
    // Lower current moon texture contrast.
    if (planet->id == MOON) painter.contrast = 0.6;

    // Set the min required depth range needed with some margins for the
    // actual planet size and the rings.
    dist = vec3_norm(pvo[0]);
    depth_range[0] = dist * 0.5;
    depth_range[1] = dist * 2;
    painter.depth_range = &depth_range;

    // Compute the required split order, based on the size of the planet
    // on screen.
    pixel_size = angle * painter.proj->window_size[0] /
                 painter.proj->scaling[0] / 2;
    split_order = ceil(mix(2, 5, smoothstep(100, 600, pixel_size)));

    hips_render_traverse(hips, &painter, mat, angle, split_order,
                         USER_PASS(planet, &nb_tot, &nb_loaded),
                         on_render_tile);
    if (planet->rings.tex)
        render_rings(planet, &painter, mat);
    progressbar_report(planet->name, planet->name, nb_loaded, nb_tot, -1);
}

/*
 * Compute Kepler orbit elements of a planet in ICRF, centered on the parent
 * body.
 */
static void planet_compute_orbit_elements(
        const planet_t *planet,
        const observer_t *obs,
        double *in,
        double *om,
        double *w,
        double *a,
        double *n,
        double *ec,
        double *ma)
{
    const double G = 6.674e-11;
    const double SPD = 60 * 60 * 24;
    double p[3], v[3];
    // μ in (AU)³(day)⁻²
    double mu = G * planet->parent->mass / (DAU * DAU * DAU) * SPD * SPD;
    double pvh[2][3], parent_pvh[2][3];
    planet_get_pvh(planet->parent, obs, parent_pvh);
    planet_get_pvh(planet, obs, pvh);
    vec3_sub(pvh[0], parent_pvh[0], p);
    vec3_sub(pvh[1], parent_pvh[1], v);
    orbit_elements_from_pv(p, v, mu, in, om, w, a, n, ec, ma);
}


static void planet_render_orbit(const planet_t *planet,
                                double alpha,
                                const painter_t *painter_)
{
    painter_t painter = *painter_;
    double mat[4][4] = MAT4_IDENTITY, parent_pvo[2][3];
    double dist, depth_range[2];
    double in, om, w, a, n, ec, ma;

    if (planet->color[3]) vec3_copy(planet->color, painter.color);
    painter.color[3] *= alpha;

    planet_compute_orbit_elements(planet, painter.obs, &in, &om, &w, &a, &n,
                                  &ec, &ma);

    // Center the rendering on the parent planet.
    planet_get_pvo(planet->parent, painter.obs, parent_pvo, true);
    mat4_itranslate(mat, parent_pvo[0][0], parent_pvo[0][1], parent_pvo[0][2]);

    // Set the depth range same as the parent!!!!
    dist = vec3_norm(parent_pvo[0]);
    depth_range[0] = dist * 0.5;
    depth_range[1] = dist * 2;
    painter.depth_range = &depth_range;

    painter.lines.width = 1;
    paint_orbit(&painter, FRAME_ICRF, mat, painter.obs->tt,
                in, om, w, a, n, ec, ma);
}

static void planet_render_label(
        const planet_t *planet, const painter_t *painter, double vmag,
        double scale, double point_size)
{
    const double label_color[4] = RGBA(223, 223, 255, 255);
    const double white[4] = {1, 1, 1, 1};
    double s, radius;
    double pos[3];
    const char *name;
    bool selected = core->selection && planet->obj.oid == core->selection->oid;
    double pvo[2][3];

    name = sys_translate("skyculture", planet->name);

    planet_get_pvo(planet, painter->obs, pvo, true);
    vec3_copy(pvo[0], pos);
    vec3_normalize(pos, pos);

    // Radius on screen in pixel.
    radius = planet->radius_m / DAU / vec3_norm(pvo[0]);
    radius *= painter->proj->window_size[0] / painter->proj->scaling[0] / 2;
    radius *= scale;

    s = point_size / 2;
    s = max(s, radius);

    labels_add_3d(name, FRAME_ICRF, pos,
                  true, s + 4, FONT_SIZE_BASE,
                  selected ? white : label_color, 0, 0,
                  selected ? TEXT_BOLD : TEXT_FLOAT,
                  -vmag, planet->obj.oid);
}

static void planet_render(const planet_t *planet, const painter_t *painter_)
{
    double pos[4], p_win[4];
    double color[4];
    double vmag;             // Observed magnitude.
    double point_size;       // Radius size of point (pixel).
    double point_r;          // Size (rad) and luminance if the planet is seen
    double point_luminance;  // as a point source (like a star).
    double radius_m;
    double r_scale = 1.0;    // Artificial size scale.
    double diam;                // Angular diameter (rad).
    double hips_alpha = 0;
    painter_t painter = *painter_;
    point_t point;
    double hips_k = 2.0; // How soon we switch to the hips survey.
    planets_t *planets = (planets_t*)planet->obj.parent;
    bool selected = core->selection && planet->obj.oid == core->selection->oid;
    double cap[4];
    const hips_t *hips;
    double pvo[2][3];

    if (planet->id == EARTH) return;

    vmag = planet_get_vmag(planet, painter.obs);
    if (planet->id != MOON && vmag > painter.stars_limit_mag) return;

    // Artificially increase the moon size when we are zoomed out, so that
    // we can render it as a hips survey.
    if (planet->id == MOON) {
        hips_k = 4.0;
        r_scale = max(1.0, core->fov / (20.0 * core->star_scale_screen_factor
                                        * DD2R));
    }

    core_get_point_for_mag(vmag, &point_size, &point_luminance);
    point_r = core_get_apparent_angle_for_point(painter.proj, point_size * 2.0);

    // Compute max radius of the planet, taking into account the
    // ring and the point size if it is bigger than the planet.
    radius_m = max(planet->radius_m, planet->rings.outer_radius) * r_scale;

    // Compute planet's pos and bounding cap in ICRF
    planet_get_pvo(planet, painter.obs, pvo, true);
    vec3_copy(pvo[0], pos);
    vec3_normalize(pos, pos);
    vec3_copy(pos, cap);
    cap[3] = cos(max(radius_m / DAU / vec3_norm(pvo[0]), point_r));

    if (painter_is_cap_clipped(&painter, FRAME_ICRF, cap))
        return;

    // Planet apparent diameter in rad
    diam = 2.0 * planet->radius_m / DAU / vec3_norm(pvo[0]);

    // Project planet's center
    convert_frame(painter.obs, FRAME_ICRF, FRAME_VIEW, true, pos, pos);
    project(painter.proj, PROJ_ALREADY_NORMALIZED | PROJ_TO_WINDOW_SPACE,
            pos, p_win);

    // At least 1 px of the planet is visible, report it for tonemapping
    convert_frame(painter.obs, FRAME_VIEW, FRAME_OBSERVED, true, pos, pos);
    // Exclude the sun because it is already taken into account by the
    // atmosphere luminance feedack
    if (planet->id != SUN) {
        // Ignore planets below ground
        if (pos[2] > 0)
            core_report_vmag_in_fov(vmag, diam / 2, 0);
    }

    hips = planet->hips ?: planets->default_hips;
    if (hips && hips_k * diam * r_scale >= point_r) {
        hips_alpha = smoothstep(1.0, 0.5, point_r / (hips_k * diam * r_scale ));
    }

    // Special case for the moon, we only render the hips, since the point
    // is much bigger than the moon.
    if (hips && planet->id == MOON) {
        hips_alpha = 1.0;
    }

    vec4_copy(planet->color, color);
    if (!color[3]) vec4_set(color, 1, 1, 1, 1);
    color[3] *= point_luminance * (1.0 - hips_alpha);

    // Lower point halo effect for objects with large radius.
    // (Mostly for the Sun, but also affect planets at large fov).
    painter.points_halo *= mix(1.0, 0.25,
                               smoothstep(0.5, 3.0, point_r * DR2D));
    point = (point_t) {
        .pos = {p_win[0], p_win[1]},
        .size = point_size,
        .color = {color[0] * 255, color[1] * 255, color[2] * 255,
                  color[3] * 255},
        .oid = planet->obj.oid,
    };
    paint_2d_points(&painter, 1, &point);

    if (hips_alpha > 0) {
        planet_render_hips(planet, hips, planet->radius_m / DAU, r_scale,
                           hips_alpha, &painter);
    }

    // Note: I force rendering the label if the hips is visible for the
    // moment because the vmag is not a good measure for planets: if the
    // planet is big on the screen, we should see the label, no matter the
    // vmag.
    if (selected || vmag <= painter.hints_limit_mag - 1.0 || hips_alpha > 0)
        planet_render_label(planet, &painter, vmag, r_scale, point_size);

    // Render the Sun halo.
    if (planet->id == SUN) {
        // Modulate halo opacity according to sun's altitude
        // This is ad-hoc code to be replaced when proper extinction is
        // computed.
        vec4_set(color, 1, 1, 1, fabs(pos[2]));
        paint_texture(&painter, planets->halo_tex, NULL, p_win, 200.0, color,
                      0);
    }
}

static int sort_cmp(const obj_t *a, const obj_t *b)
{
    const planet_t *pa = (const planet_t*)a;
    const planet_t *pb = (const planet_t*)b;
    double apvo[2][3], bpvo[2][3];
    observer_t *obs = core->observer;
    planet_get_pvo(pa, obs, apvo, true);
    planet_get_pvo(pb, obs, bpvo, true);
    return cmp(eraPm(bpvo[0]), eraPm(apvo[0]));
}

static double planet_get_pixel_radius(const planet_t *p,
                                      const painter_t *painter)
{
    double vmag, angle, pvo[2][3], radius_physical, radius_vmag;

    planet_get_pvo(p, painter->obs, pvo, true);
    angle = p->radius_m / DAU / vec3_norm(pvo[0]);
    radius_physical = core_get_point_for_apparent_angle(painter->proj, angle);
    vmag = planet_get_vmag(p, painter->obs);
    core_get_point_for_mag(vmag, &radius_vmag, NULL);
    return max(radius_physical, radius_vmag);
}

/*
* Heuristic to decide if we should render the orbit of a planet.
*/
static bool should_render_orbit(const planet_t *p, const painter_t *painter)
{

    // Only consider planets moons.
    if (!core->selection) return false;
    if (!p->parent) return false;
    if (p->id == SUN) return false;
    if (p->parent->id == SUN) return false;

    // If the moon is selected, always render the orbit.
    if (p->obj.oid == core->selection->oid) return true;

    // If the parent is not selected, don't render.
    if (p->parent->obj.oid != core->selection->oid) return false;

    // Only render the orbit if the visible radius on screen is larger than
    // a threshold value.
    if (planet_get_pixel_radius(p, painter) < 1.5) return false;

    return true;
}

static int planets_render(const obj_t *obj, const painter_t *painter)
{
    PROFILE(planets_render, 0);
    planets_t *planets = (planets_t*)obj;
    planet_t *p;

    // First sort all the planets by distance to the observer.
    DL_SORT(planets->obj.children, sort_cmp);

    if (planets->visible.value <= 0) return 0;
    painter_t painter_ = *painter;
    painter_.color[3] = planets->visible.value;
    PLANETS_ITER(planets, p) {
        planet_render(p, &painter_);
    }

    // Render orbits after the planets for proper depth buffer.
    // Note: the renderer could sort it itself?
    // Still disabled by default for the moment.
    if ((0)) // Disabled for the moment
    PLANETS_ITER(planets, p) {
        p->orbit_visible.target = should_render_orbit(p, painter);
        if (p->orbit_visible.value)
            planet_render_orbit(p, 0.6 * p->orbit_visible.value, painter);
    }
    return 0;
}

static obj_t *planets_get_by_oid(
        const obj_t *obj, uint64_t oid, uint64_t hint)
{
    obj_t *child;
    if (!oid_is_catalog(oid, "HORI")) return NULL;
    MODULE_ITER(obj, child, "planet") {
        if (child->oid == oid) {
            child->ref++;
            return child;
        }
    }
    return NULL;
}

// Parse an orbit line as returned by HORIZONS online service.
static int parse_orbit(planet_t *p, const char *v)
{
    int r;
    double jd, ec, qr, in, om, w, tp, n, ma, ta, a, ad, pr;
    if (!str_startswith(v, "horizons:")) return 0;
    r = sscanf(v, "horizons:%lf, A.D. %*s %*s "
               "%lf, %lf, %lf, %lf, %lf, %lf, %lf, %lf, %lf, %lf, %lf, %lf",
               &jd, &ec, &qr, &in, &om, &w, &tp, &n, &ma, &ta, &a, &ad, &pr);
    if (r != 13) {
        LOG_E("Cannot parse orbit line '%s'", v);
        return -1;
    }
    p->orbit.mjd = jd - 2400000.5;
    p->orbit.in = in * DD2R;
    p->orbit.om = om * DD2R;
    p->orbit.w = w * DD2R;
    p->orbit.a = a * (1000.0 / DAU);
    p->orbit.n = n * DD2R * 60 * 60 * 24;
    p->orbit.ec = ec;
    p->orbit.ma = ma * DD2R;

    // Make sure the epoch was in MJD, and not in JD.
    assert(fabs(p->orbit.mjd - DJM00) < DJY * 100);

    return 0;
}

/*
 * Conveniance function to look for a planet by name
 */
static planet_t *planet_get_by_name(planets_t *planets, const char *name)
{
    planet_t *p;
    PLANETS_ITER(planets, p) {
        if (strcasecmp(p->name, name) == 0)
            return p;
    }
    return NULL;
}

// Parse the planet data.
static int planets_ini_handler(void* user, const char* section,
                               const char* attr, const char* value)
{
    planets_t *planets = user;
    planet_t *planet;
    char id[64], name[64], unit[32];
    float v;

    str_to_upper(section, id);

    planet = planet_get_by_name(planets, section);
    if (!planet) {
        planet = (void*)module_add_new(&planets->obj, "planet", id, NULL);
        strcpy(name, section);
        name[0] += 'A' - 'a';
        planet->name = strdup(name);
        if (strcmp(id, "SUN") == 0) planets->sun = planet;
        if (strcmp(id, "EARTH") == 0) planets->earth = planet;
        planet->update_delta_s = 1.f + 1.f * rand() * 1.f / RAND_MAX;
        fader_init(&planet->orbit_visible, false);
    }
    if (strcmp(attr, "horizons_id") == 0) {
        sscanf(value, "%d", &planet->id);
        planet->obj.oid = oid_create("HORI", planet->id);
    }
    if (strcmp(attr, "type") == 0) {
        strncpy(planet->obj.type, value, 4);
    }
    if (strcmp(attr, "radius") == 0) {
        sscanf(value, "%f km", &v);
        planet->radius_m = v * 1000;
    }
    if (strcmp(attr, "parent") == 0) {
        planet->parent = planet_get_by_name(planets, value);
        assert(planet->parent);
    }
    if (strcmp(attr, "color") == 0) {
        sscanf(value, "%lf, %lf, %lf",
               &planet->color[0], &planet->color[1], &planet->color[2]);
        planet->color[3] = 1.0;
    }
    if (strcmp(attr, "albedo") == 0) {
        sscanf(value, "%f", &v);
        planet->albedo = v;
    }
    if (strcmp(attr, "rot_obliquity") == 0) {
        if (sscanf(value, "%f deg", &v) != 1) goto error;
        planet->rot.obliquity = v * DD2R;
    }
    if (strcmp(attr, "rot_period") == 0) {
        if (sscanf(value, "%f %s", &v, unit) != 2) goto error;
        if (strcmp(unit, "h") == 0) v /= 24.0;
        planet->rot.period = v;
    }
    if (strcmp(attr, "rot_offset") == 0) {
        sscanf(value, "%f", &v);
        planet->rot.offset = v * DD2R;
    }
    if (strcmp(attr, "rot_pole_ra") == 0) {
        sscanf(value, "%f", &v);
        planet->rot.pole_ra = v * DD2R;
    }
    if (strcmp(attr, "rot_pole_de") == 0) {
        sscanf(value, "%f", &v);
        planet->rot.pole_de = v * DD2R;
    }
    if (strcmp(attr, "rings_inner_radius") == 0) {
        sscanf(value, "%f km", &v);
        planet->rings.inner_radius = v * 1000;
    }
    if (strcmp(attr, "rings_outer_radius") == 0) {
        sscanf(value, "%f km", &v);
        planet->rings.outer_radius = v * 1000;
    }
    if (strcmp(attr, "orbit") == 0) {
        parse_orbit(planet, value);
    }
    if (strcmp(attr, "mass") == 0) {
        sscanf(value, "%lg kg", &planet->mass);
    }

    return 0;

error:
    LOG_W("Cannot parse planet attribute: [%s] %s = %s",
          section, attr, value);
    return -1;
}

static int planets_init(obj_t *obj, json_value *args)
{
    planets_t *planets = (void*)obj;
    char *data, name[128];
    const char *path;
    int r;
    planet_t *p;
    regex_t reg;
    regmatch_t matches[2];

    fader_init(&planets->visible, true);

    data = asset_get_data("asset://planets.ini", NULL, NULL);
    ini_parse_string(data, planets_ini_handler, planets);
    assert(planets->sun);
    assert(planets->earth);

    // Add rings textures from assets.
    regcomp(&reg, "^.*/([^/]+)_rings.png$", REG_EXTENDED);
    ASSET_ITER("asset://textures/", path) {
        r = regexec(&reg, path, 2, matches, 0);
        if (r) continue;
        snprintf(name, sizeof(name), "%.*s",
                 (int)(matches[1].rm_eo - matches[1].rm_so),
                 path + matches[1].rm_so);
        p = planet_get_by_name(planets, name);
        if (!p) continue;
        p->rings.tex = texture_from_url(path, TF_LAZY_LOAD);
    }
    regfree(&reg);

    planets->earth_shadow_tex =
        texture_from_url("asset://textures/earth_shadow.png", TF_LAZY_LOAD);
    planets->halo_tex =
        texture_from_url("asset://textures/halo.png", TF_LAZY_LOAD);

    return 0;
}

static int planets_update(obj_t *obj, double dt)
{
    planets_t *planets = (void*)obj;
    planet_t *p;

    fader_update(&planets->visible, dt);
    PLANETS_ITER(obj, p) {
        fader_update(&p->orbit_visible, dt);
    }
    return 0;
}

static obj_t *planets_get(const obj_t *obj, const char *id, int flags)
{
    planet_t *p;
    if (!str_startswith(id, "PLANET")) return NULL;
    PLANETS_ITER(obj, p) {
        if (strcmp(p->obj.id, id) == 0) {
            p->obj.ref++;
            return &p->obj;
        }
    }
    return NULL;
}


static int planets_add_data_source(
        obj_t *obj, const char *url, const char *type, json_value *args)
{
    const char *args_type, *frame, *release_date_str;
    double release_date = 0;
    planets_t *planets = (void*)obj;
    planet_t *p;

    if (!type || !args || strcmp(type, "hips")) return 1;
    args_type = json_get_attr_s(args, "type");
    if (!args_type) return 1;
    if (strcmp(args_type, "planet") && strcmp(args_type, "planet-normal"))
        return 1;

    frame = json_get_attr_s(args, "hips_frame");
    if (!frame) return 1;
    release_date_str = json_get_attr_s(args, "hips_release_date");
    if (release_date_str)
        release_date = hips_parse_date(release_date_str);

    if (strcmp(frame, "default") == 0) {
        planets->default_hips = hips_create(url, release_date, NULL);
        hips_set_frame(planets->default_hips, FRAME_ICRF);
        return 1;
    }

    p = planet_get_by_name(planets, frame);
    if (!p) return 1;
    if (strcmp(args_type, "planet") == 0) {
        p->hips = hips_create(url, release_date, NULL);
        hips_set_frame(p->hips, FRAME_ICRF);
    } else {
        p->hips_normalmap = hips_create(url, release_date, NULL);
        hips_set_frame(p->hips_normalmap, FRAME_ICRF);
    }
    return 0;
}


/*
 * Meta class declarations.
 */

static obj_klass_t planet_klass = {
    .id = "planet",
    .model = "jpl_sso",
    .size = sizeof(planet_t),
    .get_info = planet_get_info,
    .get_designations = planet_get_designations,
};
OBJ_REGISTER(planet_klass)

static obj_klass_t planets_klass = {
    .id     = "planets",
    .size   = sizeof(planets_t),
    .flags  = OBJ_IN_JSON_TREE | OBJ_MODULE | OBJ_LISTABLE,
    .init   = planets_init,
    .update = planets_update,
    .render = planets_render,
    .get_by_oid = planets_get_by_oid,
    .get     = planets_get,
    .add_data_source = planets_add_data_source,
    .render_order = 30,
    .attributes = (attribute_t[]) {
        PROPERTY(visible, TYPE_BOOL, MEMBER(planets_t, visible.target)),
        {}
    },
};
OBJ_REGISTER(planets_klass)
