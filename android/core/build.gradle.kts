import org.gradle.api.tasks.testing.TestDescriptor
import org.gradle.api.tasks.testing.TestListener
import org.gradle.api.tasks.testing.TestResult

plugins {
    alias(libs.plugins.kotlin.jvm)
}

// 编译器是 JBR 25，字节码仍对齐 17。不用 jvmToolchain：这台机器只有 Studio 自带的 JBR。
java {
    sourceCompatibility = JavaVersion.VERSION_17
    targetCompatibility = JavaVersion.VERSION_17
}

kotlin {
    compilerOptions {
        jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17)
    }
}

dependencies {
    implementation(libs.juniversalchardet)
    implementation(libs.zero.allocation.hashing)
    testImplementation(libs.junit.jupiter)
    testRuntimeOnly(libs.junit.platform.launcher)
}

tasks.test {
    useJUnitPlatform {
        excludeTags("large")
    }
    addTestListener(summaryListener())
}

tasks.register<Test>("largeFileTest") {
    group = "verification"
    description = "现场生成约 20MB TXT，测量导入耗时和堆峰值"
    testClassesDirs = sourceSets["test"].output.classesDirs
    classpath = sourceSets["test"].runtimeClasspath
    useJUnitPlatform {
        includeTags("large")
    }
    // 堆上限收紧，避免把整本复制进多个大数组还能蒙混过关。
    maxHeapSize = "256m"
    shouldRunAfter(tasks.named("test"))
    testLogging {
        showStandardStreams = true
    }
    addTestListener(summaryListener())
}

fun summaryListener() = object : TestListener {
    override fun beforeSuite(suite: TestDescriptor) = Unit
    override fun afterSuite(suite: TestDescriptor, result: TestResult) {
        if (suite.parent == null) {
            println(
                "结果: ${result.testCount} 个测试，通过 ${result.successfulTestCount}，" +
                    "失败 ${result.failedTestCount}，跳过 ${result.skippedTestCount}",
            )
        }
    }
    override fun beforeTest(testDescriptor: TestDescriptor) = Unit
    override fun afterTest(testDescriptor: TestDescriptor, result: TestResult) = Unit
}
