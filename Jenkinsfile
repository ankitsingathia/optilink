// Jenkins pipeline for OptiLink.
//
// Mirrors .github/workflows/ci.yml stage for stage. Both exist because the
// GitHub workflow is what runs on every push, while this is what the same
// project looks like inside an environment that builds on its own hardware,
// which is where firmware usually ends up: the box with the board attached is
// not a cloud runner.
//
// Every stage shells out to the same make targets the workflow and
// scripts/run_tests.sh use, so there is exactly one definition of what
// "passing" means and no chance of the two drifting apart.

pipeline {
    agent any

    options {
        timestamps()
        timeout(time: 30, unit: 'MINUTES')
        buildDiscarder(logRotator(numToKeepStr: '30'))
    }

    environment {
        FLASH_LIMIT = '131072'
        RAM_LIMIT   = '32768'
    }

    stages {
        stage('Host build') {
            steps {
                sh 'make host'
            }
        }

        stage('Tests') {
            // Unit, integration and sanitizer suites are independent, so
            // running them together shortens the feedback loop on the
            // agent without changing what is checked.
            parallel {
                stage('C unit tests') {
                    steps {
                        sh 'make test'
                    }
                }
                stage('Integration') {
                    steps {
                        sh 'python3 -m pip install --user -r requirements.txt'
                        sh 'python3 -m pytest tests/integration -v --junitxml=build/pytest.xml'
                    }
                    post {
                        always {
                            junit allowEmptyResults: true, testResults: 'build/pytest.xml'
                        }
                    }
                }
                stage('Sanitizers') {
                    steps {
                        sh 'make asan'
                    }
                }
            }
        }

        stage('Traffic reconciliation') {
            steps {
                sh 'python3 tools/trafficgen.py --frames 5000 --corrupt-rate 0.15'
            }
        }

        stage('Target cross build') {
            steps {
                sh 'make arm'
                sh 'make size-check'
            }
            post {
                success {
                    archiveArtifacts artifacts: 'build/arm/optilink.elf,build/arm/optilink.map',
                                     fingerprint: true
                }
            }
        }

        stage('LinkMon self-test') {
            steps {
                sh 'dotnet run --project tools/LinkMon -- --self-test'
            }
        }
    }

    post {
        cleanup {
            sh 'make clean || true'
        }
    }
}
