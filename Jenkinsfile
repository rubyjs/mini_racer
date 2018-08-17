def test(version, env='') {
    node('agent_ruby_build') {
        unstash 'git'

        sh "${env} ./test_me.sh ${version}"
    }
}

try {

    stage('Checkout scm') {
        node('agent_ruby_build') {
            checkout scm

            sh 'git clean -ffdx'

            stash includes: '**/*', name: 'git'
        }
    }

    stage('Testing') {
        parallel(failFast: false,
            "1.9.3-p551"    : { test("1.9.3-p551") },
            "2.0.0-p647"    : { test("2.0.0-p647") },
            "2.2.3"         : { test("2.2.3") },
            "2.3.3"         : { test("2.3.3") },
            "2.4.3"         : { test("2.4.3") },
            "2.5.0"         : { test("2.5.0") },
        )
    }

    stage('Building 2.3.3') {
        node('agent_ruby_build') {
            sh './build_me.sh 2.3.3'

            if (env.BRANCH_NAME == 'master') {
                archiveArtifacts 'pkg/sq_mini_racer-*.gem'
            }
        }
    }
} catch (e) {
    currentBuild.result = "FAILED"
    notifyFailed()
    throw e
}


def notifyFailed() {
    slackSend(color: '#FF0000', message: "FAILED: Job '${env.JOB_NAME} [${env.BUILD_NUMBER}]' (${env.BUILD_URL})")
}
