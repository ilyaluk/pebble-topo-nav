/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -  */
/* Geodesy representation conversion functions                        (c) Chris Veness 2002-2020  */
/*                                                                                   MIT Licence  */
/* www.movable-type.co.uk/scripts/latlong.html                                                    */
/* www.movable-type.co.uk/scripts/js/geodesy/geodesy-library.html#dms                             */
/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -  */

/* eslint no-irregular-whitespace: [2, { skipComments: true }] */


/**
 * Latitude/longitude points may be represented as decimal degrees, or subdivided into sexagesimal
 * minutes and seconds. This module provides methods for parsing and representing degrees / minutes
 * / seconds.
 *
 * @module dms
 */


/* Degree-minutes-seconds (& cardinal directions) separator character */
let dmsSeparator = '\u202f'; // U+202F = 'narrow no-break space'


/**
 * Functions for parsing and representing degrees / minutes / seconds.
 */
class Dms {

    // note Unicode Degree = U+00B0. Prime = U+2032, Double prime = U+2033

    /**
     * Separator character to be used to separate degrees, minutes, seconds, and cardinal directions.
     *
     * Default separator is U+202F ‘narrow no-break space’.
     *
     * To change this (e.g. to empty string or full space), set Dms.separator prior to invoking
     * formatting.
     *
     * @example
     *   import LatLon, { Dms } from '/js/geodesy/latlon-spherical.js';
     *   const p = new LatLon(51.2, 0.33).toString('dms');  // 51° 12′ 00″ N, 000° 19′ 48″ E
     *   Dms.separator = '';                                // no separator
     *   const pʹ = new LatLon(51.2, 0.33).toString('dms'); // 51°12′00″N, 000°19′48″E
     */
    static get separator()     { return dmsSeparator; }
    static set separator(char) { dmsSeparator = char; }


    /**
     * Parses string representing degrees/minutes/seconds into numeric degrees.
     *
     * This is very flexible on formats, allowing signed decimal degrees, or deg-min-sec optionally
     * suffixed by compass direction (NSEW); a variety of separators are accepted. Examples -3.62,
     * '3 37 12W', '3°37′12″W'.
     *
     * Thousands/decimal separators must be comma/dot; use Dms.fromLocale to convert locale-specific
     * thousands/decimal separators.
     *
     * @param   {string|number} dms - Degrees or deg/min/sec in variety of formats.
     * @returns {number}        Degrees as decimal number.
     *
     * @example
     *   const lat = Dms.parse('51° 28′ 40.37″ N');
     *   const lon = Dms.parse('000° 00′ 05.29″ W');
     *   const p1 = new LatLon(lat, lon); // 51.4779°N, 000.0015°W
     */
    static parse(dms) {
        // check for signed decimal degrees without NSEW, if so return it directly
        if (!isNaN(parseFloat(dms)) && isFinite(dms)) return Number(dms);

        // strip off any sign or compass dir'n & split out separate d/m/s
        const dmsParts = String(dms).trim().replace(/^-/, '').replace(/[NSEW]$/i, '').split(/[^0-9.,]+/);
        if (dmsParts[dmsParts.length-1]=='') dmsParts.splice(dmsParts.length-1);  // from trailing symbol

        if (dmsParts == '') return NaN;

        // and convert to decimal degrees...
        let deg = null;
        switch (dmsParts.length) {
            case 3:  // interpret 3-part result as d/m/s
                deg = dmsParts[0]/1 + dmsParts[1]/60 + dmsParts[2]/3600;
                break;
            case 2:  // interpret 2-part result as d/m
                deg = dmsParts[0]/1 + dmsParts[1]/60;
                break;
            case 1:  // just d (possibly decimal) or non-separated dddmmss
                deg = dmsParts[0];
                // check for fixed-width unseparated format eg 0033709W
                //if (/[NS]/i.test(dmsParts)) deg = '0' + deg;  // - normalise N/S to 3-digit degrees
                //if (/[0-9]{7}/.test(deg)) deg = deg.slice(0,3)/1 + deg.slice(3,5)/60 + deg.slice(5)/3600;
                break;
            default:
                return NaN;
        }
        if (/^-|[WS]$/i.test(dms.trim())) deg = -deg; // take '-', west and south as -ve

        return Number(deg);
    }

    /**
     * Constrain degrees to range -90..+90 (for latitude); e.g. -91 => -89, 91 => 89.
     *
     * @private
     * @param {number} degrees
     * @returns degrees within range -90..+90.
     */
    static wrap90(degrees) {
        if (-90<=degrees && degrees<=90) return degrees; // avoid rounding due to arithmetic ops if within range

        // latitude wrapping requires a triangle wave function; a general triangle wave is
        //     f(x) = 4a/p ⋅ | (x-p/4)%p - p/2 | - a
        // where a = amplitude, p = period, % = modulo; however, JavaScript '%' is a remainder operator
        // not a modulo operator - for modulo, replace 'x%n' with '((x%n)+n)%n'
        const x = degrees, a = 90, p = 360;
        return 4*a/p * Math.abs((((x-p/4)%p)+p)%p - p/2) - a;
    }

    /**
     * Constrain degrees to range -180..+180 (for longitude); e.g. -181 => 179, 181 => -179.
     *
     * @private
     * @param {number} degrees
     * @returns degrees within range -180..+180.
     */
    static wrap180(degrees) {
        if (-180<=degrees && degrees<=180) return degrees; // avoid rounding due to arithmetic ops if within range

        // longitude wrapping requires a sawtooth wave function; a general sawtooth wave is
        //     f(x) = (2ax/p - p/2) % p - a
        // where a = amplitude, p = period, % = modulo; however, JavaScript '%' is a remainder operator
        // not a modulo operator - for modulo, replace 'x%n' with '((x%n)+n)%n'
        const x = degrees, a = 180, p = 360;
        return (((2*a*x/p - p/2)%p)+p)%p - a;
    }

    /**
     * Constrain degrees to range 0..360 (for bearings); e.g. -1 => 359, 361 => 1.
     *
     * @private
     * @param {number} degrees
     * @returns degrees within range 0..360.
     */
    static wrap360(degrees) {
        if (0<=degrees && degrees<360) return degrees; // avoid rounding due to arithmetic ops if within range

        // bearing wrapping requires a sawtooth wave function with a vertical offset equal to the
        // amplitude and a corresponding phase shift; this changes the general sawtooth wave function from
        //     f(x) = (2ax/p - p/2) % p - a
        // to
        //     f(x) = (2ax/p) % p
        // where a = amplitude, p = period, % = modulo; however, JavaScript '%' is a remainder operator
        // not a modulo operator - for modulo, replace 'x%n' with '((x%n)+n)%n'
        const x = degrees, a = 180, p = 360;
        return (((2*a*x/p)%p)+p)%p;
    }

}


// Extend Number object with methods to convert between degrees & radians
Number.prototype.toRadians = function() { return this * Math.PI / 180; };
Number.prototype.toDegrees = function() { return this * 180 / Math.PI; };


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -  */

module.exports = Dms;
