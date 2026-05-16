// Copyright 2020-2026 The Defold Foundation
// Copyright 2014-2020 King
// Copyright 2009-2014 Ragnar Svensson, Christian Murray
// Licensed under the Defold License version 1.0 (the "License"); you may not use
// this file except in compliance with the License.
//
// You may obtain a copy of the License, together with FAQs at
// https://www.defold.com/license
//
// Unless required by applicable law or agreed to in writing, software distributed
// under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

package com.dynamo.bob.bundle;

import java.io.File;
import java.io.IOException;
import java.util.List;
import java.util.Map;

import org.apache.commons.io.FileUtils;
import org.apache.commons.io.FilenameUtils;

import com.dynamo.bob.Bob;
import com.dynamo.bob.CompileExceptionError;
import com.dynamo.bob.Platform;
import com.dynamo.bob.Project;
import com.dynamo.bob.fs.IResource;
import com.dynamo.bob.pipeline.ExtenderUtil;
import com.dynamo.bob.util.BobProjectProperties;

// Minimum-viable bundler for HarmonyOS Next / OpenHarmony arm64-ohos.
//
// Bob's bundler dispatch needs a registered IBundler per platform pair,
// otherwise it errors with "No bundler registered for platform
// arm64-ohos" before the user even sees their engine .so. This class
// gives the dispatch something to find, and produces a flat output
// directory (engine .so + archive) that the project's ohos/ ArkTS
// shell consumes via `hvigorw assembleHap`.
//
// A full HAP producer (templating module.json5, signing with
// hap-sign-tool.jar, packaging) is intentionally not here — that has
// to wait until the engine platform-layer port (FORK_NOTES.md §3)
// produces a real libdmengine.so for arm64-ohos to package.
@BundlerParams(platforms = {"arm64-ohos"})
public class OhosBundler implements IBundler {

    @Override
    public IResource getManifestResource(Project project, Platform platform) throws IOException {
        return null;
    }

    @Override
    public String getMainManifestName(Platform platform) {
        return null;
    }

    @Override
    public String getMainManifestTargetPath(Platform platform) {
        return null;
    }

    @Override
    public void updateManifestProperties(Project project, Platform platform,
                                BobProjectProperties projectProperties,
                                Map<String, Map<String, Object>> propertiesMap,
                                Map<String, Object> properties) throws IOException {
    }

    @Override
    public void bundleApplication(Project project, Platform platform, File bundleDir, ICanceled canceled)
            throws IOException, CompileExceptionError {

        final List<Platform> architectures = Platform.getArchitecturesFromString(
                project.option("architectures", ""), platform);

        BobProjectProperties projectProperties = project.getProjectProperties();
        String title = projectProperties.getStringValue("project", "title", "Unnamed");
        String exeName = BundleHelper.projectNameToBinaryName(title);
        File appDir = new File(bundleDir, title);

        BundleHelper.throwIfCanceled(canceled);

        FileUtils.deleteDirectory(appDir);
        appDir.mkdirs();

        BundleHelper.throwIfCanceled(canceled);

        // Copy the engine library (libdmengine.so or extender-rebuilt
        // libdmengine_<sha>.so). On OHOS the engine lives in a .so
        // loaded by the ArkTS entry, not a stand-alone executable.
        File buildDir = new File(project.getRootDirectory(), project.getBuildDirectory());
        List<File> bundleLibs = ExtenderUtil.getNativeExtensionEngineBinaries(project, platform);
        final String variant = project.option("variant", Bob.VARIANT_RELEASE);
        if (bundleLibs == null) {
            bundleLibs = Bob.getDefaultDmengineFiles(platform, variant);
        }
        if (bundleLibs.size() != 1) {
            throw new IOException("Invalid number of binaries for arm64-ohos: " + bundleLibs.size());
        }
        File libIn = bundleLibs.get(0);
        File libOut = new File(appDir, platform.formatLibraryName(exeName));
        FileUtils.copyFile(libIn, libOut);
        libOut.setExecutable(true);

        BundleHelper.throwIfCanceled(canceled);

        File binaryDir = new File(FilenameUtils.concat(
                project.getBinaryOutputDirectory(), platform.getExtenderPair()));
        BundleHelper.copySharedLibraries(platform, binaryDir, appDir);

        BundleHelper.throwIfCanceled(canceled);

        if (BundleHelper.isArchiveIncluded(project)) {
            for (String name : BundleHelper.getArchiveFilenames(buildDir)) {
                FileUtils.copyFile(new File(buildDir, name), new File(appDir, name));
            }
        }

        BundleHelper.throwIfCanceled(canceled);

        Map<String, IResource> bundleResources = ExtenderUtil.collectBundleResources(project, architectures);
        ExtenderUtil.writeResourcesToDirectory(bundleResources, appDir);

        BundleHelper.moveBundleIfNeed(project, bundleDir);
    }
}
