/**
* This file is part of DSO.
* 
* Copyright 2016 Technical University of Munich and Intel.
* Developed by Jakob Engel <engelj at in dot tum dot de>,
* for more information see <http://vision.in.tum.de/dso>.
* If you use this code, please cite the respective publications as
* listed on the above website.
*
* DSO is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* DSO is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with DSO. If not, see <http://www.gnu.org/licenses/>.
*/



#include "FullSystem/ImmaturePoint.h"
#include "util/FrameShell.h"
#include "FullSystem/ResidualProjections.h"

namespace dso {
    // initialize immature points.
    ImmaturePoint::ImmaturePoint(int u_, int v_, FrameHessian *host_, float type, CalibHessian *HCalib)
            : u(u_), v(v_), host(host_), my_type(type), idepth_min(0), idepth_max(NAN),
              lastTraceStatus(IPS_UNINITIALIZED) {

        gradH.setZero();
        // loop through different pattern to initialize the immature point
        for (int idx = 0; idx < patternNum; idx++) {
            // patternP is staticPattern[8] in paper
            // this loops for {0,-2},	  {-1,-1},	   {1,-1},		{-2,0},		 {0,0},		  {2,0},	   {-1,1},		{0,2}
            // which is the eight points.
            int dx = patternP[idx][0];
            int dy = patternP[idx][1];
            // dI is the image on the largest scale, three channels, color, dx, dy.
            // this ptc is a vector of bidirectional linearly interpolated [color, dx, dy]
            // collected on point in u+dx and v+dy at the original scale image plane
            Vec3f ptc = getInterpolatedElement33BiLin(host->dI, u + dx, v + dy, wG[0]);

            // see, here color takes the first value as color value, which is greyscale (normalized).
            color[idx] = ptc[0];
            // if the interpolation is infinite, which means the point is on the edge, will be OOB soon
            // thus give up on this point and return with nothing.
            if (!std::isfinite(color[idx])) {
                energyTH = NAN;
                return;
            }

            // for other valid immature points:
            // gradient H is: dx*dx + dy*dy -> H
            // H here more like a scaled abs(dx) + abs(dy) => it captures all the gradients.
            gradH += ptc.tail<2>() * ptc.tail<2>().transpose();
            // squared norm is: sqrt(x1^2 + x2^2 + ...), Frobenius norm for matrix.
            // weight of point idx is inverse propotional to the squared norm of normalized dx dy at that point
            weights[idx] = sqrtf(
                    setting_outlierTHSumComponent / (setting_outlierTHSumComponent + ptc.tail<2>().squaredNorm()));
        }
        // patternNum is 8
        // setting_outlierTH default is 12*12, higher it is, less strict the contrain is.
        // so they use 114*8 = 912 as energyTH. -> so let's see how much energy they can get for each point.
        energyTH = patternNum * setting_outlierTH;
        // setting_overallEnergyTHWeight is 1 for now. note this is a squared weighting.
        energyTH *= setting_overallEnergyTHWeight * setting_overallEnergyTHWeight;
        // idepth GT, GT here is ground truth?
        idepth_GT = 0;
        // quality is controlled and updated from point energy, the smaller the best energy is the higher the quality.
        quality = 10000;
    }

    ImmaturePoint::~ImmaturePoint() {
    }


/*
 * returns
 * * OOB -> point is optimized and marginalized
 * * UPDATED -> point has been updated.
 * * SKIP -> point has not been updated.
 */
    ImmaturePointStatus
    ImmaturePoint::traceOn(FrameHessian *frame, const Mat33f &hostToFrame_KRKi, const Vec3f &hostToFrame_Kt,
                           const Vec2f &hostToFrame_affine, CalibHessian *HCalib, bool debugPrint) {
        if (lastTraceStatus == ImmaturePointStatus::IPS_OOB) return lastTraceStatus;

        // !Notice: there's already a host frameHessian in the immature point class.
        // so this traceOn should be reproject the immature point on the upcoming frame (FrameHessian *frame).


        // just some debug information...
        debugPrint = false;//rand()%100==0;
        // setting_maxPixSearch: line segment searched during immature point tracking (settings.cpp)
        // so this maxPixSearch control how many points this traceOn want to search for candidates.
        // default is: setting_maxPixSearch = 0.027
        float maxPixSearch = (wG[0] + hG[0]) * setting_maxPixSearch;

        if (debugPrint)
            printf("trace pt (%.1f %.1f) from frame %d to %d. Range %f -> %f. t %f %f %f!\n",
                   u, v,
                   host->shell->id, frame->shell->id,
                   idepth_min, idepth_max,
                   hostToFrame_Kt[0], hostToFrame_Kt[1], hostToFrame_Kt[2]);

//	const float stepsize = 1.0;				// stepsize for initial discrete search.
//	const int GNIterations = 3;				// max # GN iterations
//	const float GNThreshold = 0.1;				// GN stop after this stepsize.
//	const float extraSlackOnTH = 1.2;			// for energy-based outlier check, be slightly more relaxed by this factor.
//	const float slackInterval = 0.8;			// if pixel-interval is smaller than this, leave it be.
//	const float minImprovementFactor = 2;		// if pixel-interval is smaller than this, leave it be.
        // ============== project min and max. return if one of them is OOB ===================
        // u, v is initialized on the constructor, which is offered by the outer function.
        // they are the image coordinates. -> this pr is point in image space.
        Vec3f pr = hostToFrame_KRKi * Vec3f(u, v, 1);
        // here pr + t*idepth_min should be the minimal inverse depth estimation t here is translation from host frame
        // to target frame.
        Vec3f ptpMin = pr + hostToFrame_Kt * idepth_min;
        // so here since, they applied the smallest idepth estimation on the point, there x and y projection will have
        // a bottom bound. which is uMin and vMin, you can image it's projected to left bottom corner of an area.
        float uMin = ptpMin[0] / ptpMin[2];
        float vMin = ptpMin[1] / ptpMin[2];
        // filter out those OOB points.
        // notice this padding is 5, why they set padding as 5? give up so many points on the border of image?
        if (!(uMin > 4 && vMin > 4 && uMin < wG[0] - 5 && vMin < hG[0] - 5)) {
            if (debugPrint)
                printf("OOB uMin %f %f - %f %f %f (id %f-%f)!\n",
                       u, v, uMin, vMin, ptpMin[2], idepth_min, idepth_max);
            // this literally set all the tracing to be bad and declare it's failing.
            lastTraceUV = Vec2f(-1, -1);
            lastTracePixelInterval = 0;
            return lastTraceStatus = ImmaturePointStatus::IPS_OOB;
        }

        float dist;
        float uMax;
        float vMax;
        Vec3f ptpMax;
        // so here is tricky. idepth_max should be finite, practically, they are only tracking obj that's
        // less than 600m+-100m, depending to the camera. which is well enough for tracking.
        if (std::isfinite(idepth_max)) {
            ptpMax = pr + hostToFrame_Kt * idepth_max;
            uMax = ptpMax[0] / ptpMax[2];
            vMax = ptpMax[1] / ptpMax[2];

            // again. OOB check, padding 5.
            if (!(uMax > 4 && vMax > 4 && uMax < wG[0] - 5 && vMax < hG[0] - 5)) {
                if (debugPrint) printf("OOB uMax  %f %f - %f %f!\n", u, v, uMax, vMax);
                lastTraceUV = Vec2f(-1, -1);
                lastTracePixelInterval = 0;
                return lastTraceStatus = ImmaturePointStatus::IPS_OOB;
            }



            // ============== check their distance. everything below 2px is OK (-> skip). ===================
            dist = (uMin - uMax) * (uMin - uMax) + (vMin - vMax) * (vMin - vMax);
            dist = sqrtf(dist);
            if (dist < setting_trace_slackInterval) {
                if (debugPrint)
                    printf("TOO CERTAIN ALREADY (dist %f)!\n", dist);

                lastTraceUV = Vec2f(uMax + uMin, vMax + vMin) * 0.5;
                lastTracePixelInterval = dist;
                return lastTraceStatus = ImmaturePointStatus::IPS_SKIPPED;
            }
            assert(dist > 0);
        } else {
            dist = maxPixSearch;

            // project to arbitrary depth to get direction.
            ptpMax = pr + hostToFrame_Kt * 0.01;
            uMax = ptpMax[0] / ptpMax[2];
            vMax = ptpMax[1] / ptpMax[2];

            // direction.
            float dx = uMax - uMin;
            float dy = vMax - vMin;
            float d = 1.0f / sqrtf(dx * dx + dy * dy);

            // set to [setting_maxPixSearch].
            uMax = uMin + dist * dx * d;
            vMax = vMin + dist * dy * d;

            // may still be out!
            if (!(uMax > 4 && vMax > 4 && uMax < wG[0] - 5 && vMax < hG[0] - 5)) {
                if (debugPrint) printf("OOB uMax-coarse %f %f %f!\n", uMax, vMax, ptpMax[2]);
                lastTraceUV = Vec2f(-1, -1);
                lastTracePixelInterval = 0;
                return lastTraceStatus = ImmaturePointStatus::IPS_OOB;
            }
            assert(dist > 0);
        }


        // set OOB if scale change too big.
        if (!(idepth_min < 0 || (ptpMin[2] > 0.75 && ptpMin[2] < 1.5))) {
            if (debugPrint) printf("OOB SCALE %f %f %f!\n", uMax, vMax, ptpMin[2]);
            lastTraceUV = Vec2f(-1, -1);
            lastTracePixelInterval = 0;
            return lastTraceStatus = ImmaturePointStatus::IPS_OOB;
        }


        // ============== compute error-bounds on result in pixel. if the new interval is not at least 1/2 of the old, SKIP ===================
        float dx = setting_trace_stepsize * (uMax - uMin);
        float dy = setting_trace_stepsize * (vMax - vMin);

        float a = (Vec2f(dx, dy).transpose() * gradH * Vec2f(dx, dy));
        float b = (Vec2f(dy, -dx).transpose() * gradH * Vec2f(dy, -dx));
        float errorInPixel = 0.2f + 0.2f * (a + b) / a;

        if (errorInPixel * setting_trace_minImprovementFactor > dist && std::isfinite(idepth_max)) {
            if (debugPrint)
                printf("NO SIGNIFICANT IMPROVMENT (%f)!\n", errorInPixel);
            lastTraceUV = Vec2f(uMax + uMin, vMax + vMin) * 0.5;
            lastTracePixelInterval = dist;
            return lastTraceStatus = ImmaturePointStatus::IPS_BADCONDITION;
        }

        if (errorInPixel > 10) errorInPixel = 10;



        // ============== do the discrete search ===================
        dx /= dist;
        dy /= dist;

        if (debugPrint)
            printf("trace pt (%.1f %.1f) from frame %d to %d. Range %f (%.1f %.1f) -> %f (%.1f %.1f)! ErrorInPixel %.1f!\n",
                   u, v,
                   host->shell->id, frame->shell->id,
                   idepth_min, uMin, vMin,
                   idepth_max, uMax, vMax,
                   errorInPixel
            );


        if (dist > maxPixSearch) {
            uMax = uMin + maxPixSearch * dx;
            vMax = vMin + maxPixSearch * dy;
            dist = maxPixSearch;
        }

        int numSteps = 1.9999f + dist / setting_trace_stepsize;
        Mat22f Rplane = hostToFrame_KRKi.topLeftCorner<2, 2>();

        float randShift = uMin * 1000 - floorf(uMin * 1000);
        float ptx = uMin - randShift * dx;
        float pty = vMin - randShift * dy;


        Vec2f rotatetPattern[MAX_RES_PER_POINT];
        for (int idx = 0; idx < patternNum; idx++)
            rotatetPattern[idx] = Rplane * Vec2f(patternP[idx][0], patternP[idx][1]);


        if (!std::isfinite(dx) || !std::isfinite(dy)) {
            //printf("COUGHT INF / NAN dxdy (%f %f)!\n", dx, dx);

            lastTracePixelInterval = 0;
            lastTraceUV = Vec2f(-1, -1);
            return lastTraceStatus = ImmaturePointStatus::IPS_OOB;
        }


        float errors[100];
        float bestU = 0, bestV = 0, bestEnergy = 1e10;
        int bestIdx = -1;
        if (numSteps >= 100) numSteps = 99;

        for (int i = 0; i < numSteps; i++) {
            float energy = 0;
            for (int idx = 0; idx < patternNum; idx++) {
                float hitColor = getInterpolatedElement31(frame->dI,
                                                          (float) (ptx + rotatetPattern[idx][0]),
                                                          (float) (pty + rotatetPattern[idx][1]),
                                                          wG[0]);

                if (!std::isfinite(hitColor)) {
                    energy += 1e5;
                    continue;
                }
                float residual = hitColor - (float) (hostToFrame_affine[0] * color[idx] + hostToFrame_affine[1]);
                float hw = fabs(residual) < setting_huberTH ? 1 : setting_huberTH / fabs(residual);
                energy += hw * residual * residual * (2 - hw);
            }

            if (debugPrint)
                printf("step %.1f %.1f (id %f): energy = %f!\n",
                       ptx, pty, 0.0f, energy);


            errors[i] = energy;
            if (energy < bestEnergy) {
                bestU = ptx;
                bestV = pty;
                bestEnergy = energy;
                bestIdx = i;
            }

            ptx += dx;
            pty += dy;
        }


        // find best score outside a +-2px radius.
        float secondBest = 1e10;
        for (int i = 0; i < numSteps; i++) {
            if ((i < bestIdx - setting_minTraceTestRadius || i > bestIdx + setting_minTraceTestRadius) &&
                errors[i] < secondBest)
                secondBest = errors[i];
        }
        /*
         * ////////////////////get from FullSystem.cpp////////////////////////
         * bool canActivate = (ph->lastTraceStatus == IPS_GOOD
					|| ph->lastTraceStatus == IPS_SKIPPED
					|| ph->lastTraceStatus == IPS_BADCONDITION
					|| ph->lastTraceStatus == IPS_OOB )
							&& ph->lastTracePixelInterval < 8
							&& ph->quality > setting_minTraceQuality
							&& (ph->idepth_max+ph->idepth_min) > 0;
         */
        // quality further determine weather a immature point can be activated as mature point or not.
        float newQuality = secondBest / bestEnergy; // best energy is always the smallest, that makes quality >= 1
        if (newQuality < quality || numSteps > 10) quality = newQuality;


        // ============== do GN optimization ===================
        float uBak = bestU, vBak = bestV, gnstepsize = 1, stepBack = 0;
        if (setting_trace_GNIterations > 0) bestEnergy = 1e5;
        int gnStepsGood = 0, gnStepsBad = 0;
        for (int it = 0; it < setting_trace_GNIterations; it++) {
            float H = 1, b = 0, energy = 0;
            for (int idx = 0; idx < patternNum; idx++) {
                Vec3f hitColor = getInterpolatedElement33(frame->dI,
                                                          (float) (bestU + rotatetPattern[idx][0]),
                                                          (float) (bestV + rotatetPattern[idx][1]), wG[0]);

                if (!std::isfinite((float) hitColor[0])) {
                    energy += 1e5;
                    continue;
                }
                float residual = hitColor[0] - (hostToFrame_affine[0] * color[idx] + hostToFrame_affine[1]);
                float dResdDist = dx * hitColor[1] + dy * hitColor[2];
                float hw = fabs(residual) < setting_huberTH ? 1 : setting_huberTH / fabs(residual);

                H += hw * dResdDist * dResdDist;
                b += hw * residual * dResdDist;
                energy += weights[idx] * weights[idx] * hw * residual * residual * (2 - hw);
            }


            if (energy > bestEnergy) {
                gnStepsBad++;

                // do a smaller step from old point.
                stepBack *= 0.5;
                bestU = uBak + stepBack * dx;
                bestV = vBak + stepBack * dy;
                if (debugPrint)
                    printf("GN BACK %d: E %f, H %f, b %f. id-step %f. UV %f %f -> %f %f.\n",
                           it, energy, H, b, stepBack,
                           uBak, vBak, bestU, bestV);
            } else {
                gnStepsGood++;

                float step = -gnstepsize * b / H;
                if (step < -0.5) step = -0.5;
                else if (step > 0.5) step = 0.5;

                if (!std::isfinite(step)) step = 0;

                uBak = bestU;
                vBak = bestV;
                stepBack = step;

                bestU += step * dx;
                bestV += step * dy;
                bestEnergy = energy;

                if (debugPrint)
                    printf("GN step %d: E %f, H %f, b %f. id-step %f. UV %f %f -> %f %f.\n",
                           it, energy, H, b, step,
                           uBak, vBak, bestU, bestV);
            }

            if (fabsf(stepBack) < setting_trace_GNThreshold) break;
        }


        // ============== detect energy-based outlier. ===================
//	float absGrad0 = getInterpolatedElement(frame->absSquaredGrad[0],bestU, bestV, wG[0]);
//	float absGrad1 = getInterpolatedElement(frame->absSquaredGrad[1],bestU*0.5-0.25, bestV*0.5-0.25, wG[1]);
//	float absGrad2 = getInterpolatedElement(frame->absSquaredGrad[2],bestU*0.25-0.375, bestV*0.25-0.375, wG[2]);
        if (!(bestEnergy < energyTH * setting_trace_extraSlackOnTH))
//			|| (absGrad0*areaGradientSlackFactor < host->frameGradTH
//		     && absGrad1*areaGradientSlackFactor < host->frameGradTH*0.75f
//			 && absGrad2*areaGradientSlackFactor < host->frameGradTH*0.50f))
        {
            if (debugPrint)
                printf("OUTLIER!\n");

            lastTracePixelInterval = 0;
            lastTraceUV = Vec2f(-1, -1);
            if (lastTraceStatus == ImmaturePointStatus::IPS_OUTLIER)
                return lastTraceStatus = ImmaturePointStatus::IPS_OOB;
            else
                return lastTraceStatus = ImmaturePointStatus::IPS_OUTLIER;
        }


        // ============== set new interval ===================
        if (dx * dx > dy * dy) {
            idepth_min = (pr[2] * (bestU - errorInPixel * dx) - pr[0]) /
                         (hostToFrame_Kt[0] - hostToFrame_Kt[2] * (bestU - errorInPixel * dx));
            idepth_max = (pr[2] * (bestU + errorInPixel * dx) - pr[0]) /
                         (hostToFrame_Kt[0] - hostToFrame_Kt[2] * (bestU + errorInPixel * dx));
        } else {
            idepth_min = (pr[2] * (bestV - errorInPixel * dy) - pr[1]) /
                         (hostToFrame_Kt[1] - hostToFrame_Kt[2] * (bestV - errorInPixel * dy));
            idepth_max = (pr[2] * (bestV + errorInPixel * dy) - pr[1]) /
                         (hostToFrame_Kt[1] - hostToFrame_Kt[2] * (bestV + errorInPixel * dy));
        }
        if (idepth_min > idepth_max) std::swap<float>(idepth_min, idepth_max);


        if (!std::isfinite(idepth_min) || !std::isfinite(idepth_max) || (idepth_max < 0)) {
            //printf("COUGHT INF / NAN minmax depth (%f %f)!\n", idepth_min, idepth_max);

            lastTracePixelInterval = 0;
            lastTraceUV = Vec2f(-1, -1);
            return lastTraceStatus = ImmaturePointStatus::IPS_OUTLIER;
        }

        lastTracePixelInterval = 2 * errorInPixel;
        lastTraceUV = Vec2f(bestU, bestV);
        return lastTraceStatus = ImmaturePointStatus::IPS_GOOD;
    }


    float ImmaturePoint::getdPixdd(
            CalibHessian *HCalib,
            ImmaturePointTemporaryResidual *tmpRes,
            float idepth) {
        FrameFramePrecalc *precalc = &(host->targetPrecalc[tmpRes->target->idx]);
        const Vec3f &PRE_tTll = precalc->PRE_tTll;
        float drescale, u = 0, v = 0, new_idepth;
        float Ku, Kv;
        Vec3f KliP;

        projectPoint(this->u, this->v, idepth, 0, 0, HCalib,
                     precalc->PRE_RTll, PRE_tTll, drescale, u, v, Ku, Kv, KliP, new_idepth);

        float dxdd = (PRE_tTll[0] - PRE_tTll[2] * u) * HCalib->fxl();
        float dydd = (PRE_tTll[1] - PRE_tTll[2] * v) * HCalib->fyl();
        return drescale * sqrtf(dxdd * dxdd + dydd * dydd);
    }


    float ImmaturePoint::calcResidual(
            CalibHessian *HCalib, const float outlierTHSlack,
            ImmaturePointTemporaryResidual *tmpRes,
            float idepth) {
        FrameFramePrecalc *precalc = &(host->targetPrecalc[tmpRes->target->idx]);

        float energyLeft = 0;
        const Eigen::Vector3f *dIl = tmpRes->target->dI;
        const Mat33f &PRE_KRKiTll = precalc->PRE_KRKiTll;
        const Vec3f &PRE_KtTll = precalc->PRE_KtTll;
        Vec2f affLL = precalc->PRE_aff_mode;

        for (int idx = 0; idx < patternNum; idx++) {
            float Ku, Kv;
            if (!projectPoint(this->u + patternP[idx][0], this->v + patternP[idx][1], idepth, PRE_KRKiTll, PRE_KtTll,
                              Ku, Kv)) { return 1e10; }

            Vec3f hitColor = (getInterpolatedElement33(dIl, Ku, Kv, wG[0]));
            if (!std::isfinite((float) hitColor[0])) { return 1e10; }
            //if(benchmarkSpecialOption==5) hitColor = (getInterpolatedElement13BiCub(tmpRes->target->I, Ku, Kv, wG[0]));

            float residual = hitColor[0] - (affLL[0] * color[idx] + affLL[1]);

            float hw = fabsf(residual) < setting_huberTH ? 1 : setting_huberTH / fabsf(residual);
            energyLeft += weights[idx] * weights[idx] * hw * residual * residual * (2 - hw);
        }

        if (energyLeft > energyTH * outlierTHSlack) {
            energyLeft = energyTH * outlierTHSlack;
        }
        return energyLeft;
    }


    double ImmaturePoint::linearizeResidual(
            CalibHessian *HCalib, const float outlierTHSlack,
            ImmaturePointTemporaryResidual *tmpRes,
            float &Hdd, float &bd,
            float idepth) {
        //what is Hdd?
        // what is bd?
        //
        if (tmpRes->state_state == ResState::OOB) {
            tmpRes->state_NewState = ResState::OOB;
            return tmpRes->state_energy;
        }

        FrameFramePrecalc *precalc = &(host->targetPrecalc[tmpRes->target->idx]);

        // check OOB due to scale angle change.

        float energyLeft = 0;
        const Eigen::Vector3f *dIl = tmpRes->target->dI;
        const Mat33f &PRE_RTll = precalc->PRE_RTll;
        const Vec3f &PRE_tTll = precalc->PRE_tTll;
        //const float * const Il = tmpRes->target->I;

        Vec2f affLL = precalc->PRE_aff_mode;
        // loop first 8 patterns
        for (int idx = 0; idx < patternNum; idx++) {
            // patternP use the static pattern.
            // staticPattern[10][40][2] this has 10 different patterns
            // so idx is 0-8 different pattern, the rest indice should be
            // the pattern content, right?
            // staticPatter[0][0] will be the 2
            int dx = patternP[idx][0];
            int dy = patternP[idx][1];

            float drescale, u, v, new_idepth;
            float Ku, Kv;
            Vec3f KliP;
            // this step project the immature point to the new frame.
            // if projection fails, set the state as out of boundary
            // else, go get the hitColor and calculate residual of that point.
            if (!projectPoint(this->u, this->v, idepth, dx, dy, HCalib,
                              PRE_RTll, PRE_tTll, drescale, u, v, Ku, Kv, KliP, new_idepth)) {
                tmpRes->state_NewState = ResState::OOB;
                return tmpRes->state_energy;
            }
            // !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
            // ########################## Core of Paper ####################
            // normalized color and gradient channel at Ku and Kv.
            // Ku and Kv are the reprojected point.
            Vec3f hitColor = (getInterpolatedElement33(dIl, Ku, Kv, wG[0]));

            if (!std::isfinite((float) hitColor[0])) {
                tmpRes->state_NewState = ResState::OOB;
                return tmpRes->state_energy;
            }
            // residual is propotional to the color and linearly affined by affine model.
            // affLL is affine left to left. I think here affLL[0] is a and affLL[1] is b.
            float residual = hitColor[0] - (affLL[0] * color[idx] + affLL[1]);
            // here hw is huber weights
            // if residual is less than setting_huberTH got 1
            // otherwise, will be inverse propotional to residual
            // fabsf is float absolute function.
            float hw = fabsf(residual) < setting_huberTH ? 1 : setting_huberTH / fabsf(residual);

            energyLeft += weights[idx] * weights[idx] * hw * residual * residual * (2 - hw);

            // depth derivatives.
            float dxInterp = hitColor[1] * HCalib->fxl();
            float dyInterp = hitColor[2] * HCalib->fyl();
            float d_idepth = derive_idepth(PRE_tTll, u, v, dx, dy, dxInterp, dyInterp, drescale);

            hw *= weights[idx] * weights[idx];

            Hdd += (hw * d_idepth) * d_idepth;
            bd += (hw * residual) * d_idepth;
        }


        if (energyLeft > energyTH * outlierTHSlack) {
            energyLeft = energyTH * outlierTHSlack;
            tmpRes->state_NewState = ResState::OUTLIER;
        } else {
            tmpRes->state_NewState = ResState::IN;
        }
        // assign the energy left to the temporal residual object.
        tmpRes->state_NewEnergy = energyLeft;
        return energyLeft;
    }


}
